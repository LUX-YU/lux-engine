#pragma once
/**
 * @file EditorScene.hpp
 * @brief One self-contained editor scene: World + render scene + view + features.
 *
 * `EditorScene` is the unit of "what gets unloaded when the user opens
 * another scene." It owns:
 *   - a `lux::ecs::World` (the ECS registry + built-in systems)
 *   - a `lux::render::RenderSceneId` and main offscreen `ViewHandle`
 *   - all render features added during bring-up, in registration order, so
 *     teardown can undo them in reverse
 *   - the `lux::render_bridge::RenderableSystem` that bridges the World's
 *     transform/mesh components into the renderer
 *
 * It does NOT own the `UIRenderSession` or the `AssetManager` — those are
 * process-wide and survive scene swaps; references are stored.
 *
 * Lifecycle:
 *
 *   1. construct (cheap, no GPU work)
 *   2. `bringUp(cfg)` — synchronous; issues create/register/add
 *      commands and pumps frames until each reply lands
 *   3. each frame: `processPendingResize(session)` → `tick(dt, w, h)`
 *   4. `tearDown()` — explicit; issues async removeFeature /
 *      removeUIView / destroyScene and pumps once so the render thread
 *      drains them
 *   5. destruct (must be a no-op; async render teardown cannot complete
 *      from a destructor)
 *
 * Scene swapping is not yet wired through `LuxEditor`, but the contract above
 * is what makes it safe to add: `LuxEditor::loadScene(new_cfg)` will
 * `tearDown` the current `EditorScene`, replace the `unique_ptr`, then
 * `bringUp` the new one.
 */

#include <lux/engine/editor/visibility.h>

#include <lux/engine/asset/Asset.hpp>      // asset_id_t
#include <lux/engine/math/AABB.hpp>
#include <lux/engine/meta/LuxObject.hpp>   // entity_id, null_entity
#include <lux/engine/render/core/FeatureHandle.hpp>
#include <lux/engine/render/core/RenderResourceHandle.hpp>
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/renderer/features/skinning/SkinningOperation.hpp>  // SkinningOperationIds
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>        // LightOperationIds
#include <lux/pack/d3/world/components/SceneSettingsComponent.hpp>  // ensureSceneSettings return type
#include <lux/engine/editor/scene/EditorSceneSystem.hpp>  // pluggable scene subsystems (de-hardcoded tick)
#include <lux/engine/ui/ImGuiCommConfig.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux::asset    { class AssetManager; }
namespace lux::ecs { class World; }
namespace lux::input    { class ActionMapper; }
namespace lux::render_bridge { class RenderableSystem; }
namespace lux::ui       { class UIRenderSession; class SceneViewportPanel; }

namespace lux::editor
{
    class EditorBuiltins;
    struct EditorRenderInfra;
    class CameraSceneSystem;   // non-owning ptr below (cursor-capture forward)

    struct BringUpConfig
    {
        std::string name           = "EditorScene";
        uint32_t    initial_width  = 1280;
        uint32_t    initial_height = 720;

        /// Optional path to a `.luxscene` file. If non-empty and the file
        /// exists, `bringUp` loads the entity tree from it via
        /// `lux::editor::Scene::load` after the World + RenderableSystem
        /// are wired but before features are stable, so component changes
        /// flow to the renderer through the usual adapter path.
        ///
        /// Empty path (or load failure) → World stays empty except for
        /// the editor camera. The user populates it via the inspector
        /// or `File → Scene → Open`. The editor does NOT ship any
        /// built-in demo content; that is a launcher / project-template
        /// concern, not an editor-shell concern.
        std::filesystem::path from_scene_file{};
    };

    /// Orbit-camera state — *just* the yaw/pitch/distance/target tuple plus
    /// auto-orbit speed. The camera entity carries its own intrinsic params
    /// (fov / near / far / aspect / y_flip) on its `CameraComponent`; this
    /// struct only describes "where the camera is around the target."
    ///
    /// Seeded once at bringUp (via writeOrbitToTransform) to place the camera;
    /// thereafter `CameraSceneSystem`'s `UEEditorController` drives the camera
    /// entity from input each frame, so this is just the initial-pose tuple.
    struct OrbitCameraState
    {
        float yaw_deg              = 30.f;
        float pitch_deg            = -25.f;
        float distance             = 12.f;
        float target_x             = 0.f;
        float target_y             = 1.f;
        float target_z             = 0.f;
    };

    class StateRegistry;
    class Selection;

    class LUX_EDITOR_PUBLIC EditorScene
    {
    public:
        /// @param request_load Injected async-load hook (LuxEditor wires
        ///        EngineExecutor::requestLoad). Forwarded to the
        ///        RenderableSystem bridge + the SkeletalAnimationResolver so
        ///        both materialize absent assets through the engine executor.
        EditorScene(lux::ui::UIRenderSession& session,
                    lux::asset::AssetManager& assets,
                    const EditorBuiltins&     builtins,
                    const EditorRenderInfra&  infra,
                    StateRegistry&            states,
                    std::function<void(const lux::asset::asset_id_t&)> request_load) noexcept;

        ~EditorScene();

        EditorScene(const EditorScene&)            = delete;
        EditorScene& operator=(const EditorScene&) = delete;
        EditorScene(EditorScene&&)                 = delete;
        EditorScene& operator=(EditorScene&&)      = delete;

        // ── Lifecycle ──────────────────────────────────────────────────

        /// Bring up the render-side scene + view + features synchronously.
        /// Returns false on the first failing step; on failure no commands
        /// have been issued past the failing one — call `tearDown` only if
        /// this returned true.
        [[nodiscard]] bool bringUp(const BringUpConfig& cfg);

        /// Issue async teardown commands and pump one idle frame so the
        /// render thread dispatches them. Safe to call multiple times.
        void tearDown() noexcept;

        // ── Per-frame ──────────────────────────────────────────────────

        /// Queued by the SceneViewportPanel's resize callback (which fires
        /// inside `paint()`); applied by `processPendingResize` once the
        /// next frame is open.
        void queueResize(uint32_t width, uint32_t height) noexcept;

        /// Flush a pending viewport resize via `session.resizeView`. No-op
        /// if no resize is queued.
        void processPendingResize();

        /// Advance the world by `dt`, drive the UE-style camera controller
        /// from the editor-wide `ActionMapper`, refresh the aspect ratio
        /// from `content_w / content_h`, push the camera to the renderer,
        /// then drive the RenderableSystem to bridge mesh updates.
        ///
        /// @param mapper Per-frame action snapshot from `LuxEditor::run`.
        ///               Must outlive this call but not across calls; the
        ///               controller does not retain it.
        void tick(float dt, float content_w, float content_h,
                  const lux::input::ActionMapper& mapper);

        // ── Selection — drives F-focus + viewport picking ───────────────
        //
        // Selection lives in the shared StateRegistry (one source of truth):
        // this scene READS the `Selection` for F-focus + the gizmo and WRITES it
        // from onPick. No per-scene selection state or callback.

        /// Resolve a screen-space click to an entity and write the shared
        /// `Selection` state. Coordinates are content-rect-
        /// local pixels, top-left = (0,0); @p cw / @p ch are the content size;
        /// the projection is read from the active CameraComponent. Sets the
        /// hit entity (or null on miss). No-op when not live.
        void onPick(float cx, float cy, float cw, float ch);

        /// True only while the camera is in fly mode (RMB held in the
        /// viewport). LuxEditor reads this each frame to flip the GLFW
        /// cursor between NORMAL and DISABLED.
        [[nodiscard]] bool cameraWantsCursorCapture() const noexcept;

        // ── Accessors ──────────────────────────────────────────────────

        lux::ecs::World&        world()       noexcept { return *world_; }
        const lux::ecs::World&  world() const noexcept { return *world_; }

        lux::render::RenderSceneId   sceneId()   const noexcept { return scene_id_; }
        lux::render::ViewHandle      mainView()  const noexcept { return main_view_; }

        OrbitCameraState&            orbit()        noexcept { return orbit_; }
        const OrbitCameraState&      orbit() const  noexcept { return orbit_; }

        /// The camera entity. Has TransformComponent + WorldTransformComponent
        /// + CameraComponent; CameraSystem rebuilds its view/proj each tick.
        /// Returns `null_entity` before bringUp / after tearDown.
        lux::meta::entity_id         cameraEntity() const noexcept { return camera_entity_; }

        /// True only between a successful `bringUp` and a `tearDown`.
        [[nodiscard]] bool isLive() const noexcept { return live_; }

        /// The scene-global view/streaming settings singleton (the UE WorldSettings
        /// model), creating it with struct defaults when the scene has none. The
        /// Scene Settings panel edits this in place; `tick()` dirty-applies it to
        /// streaming + the render SpatialCull.
        /// Main-thread only (mutates the registry). Precondition: scene is live.
        lux::pack::SceneSettingsComponent& ensureSceneSettings();

        /// Register a pluggable scene subsystem (streaming, anim resolve, camera,
        /// gizmo, ...). Called during bringUp. tick() runs each registered system in
        /// its phase(s) — a scene that doesn't want a capability simply omits it,
        /// instead of the old hardcoded `if (member_)` god-tick. Order matters
        /// (load-bearing); register in the intended run order.
        void registerSceneSystem(std::unique_ptr<EditorSceneSystem> sys)
        {
            if (sys) systems_.push_back(std::move(sys));
        }

    private:
        lux::ui::UIRenderSession*           session_{nullptr};
        lux::asset::AssetManager*           assets_{nullptr};
        const EditorBuiltins*               builtins_{nullptr};
        const EditorRenderInfra*            infra_{nullptr};

        std::unique_ptr<lux::ecs::World>            world_;
        std::unique_ptr<lux::render_bridge::RenderableSystem> renderable_system_;

        // Injected async-load hook (EngineExecutor::requestLoad), forwarded to the
        // RenderableSystem + the registered scene systems (anim resolver, streaming)
        // at bringUp.
        std::function<void(const lux::asset::asset_id_t&)> request_load_{};

        // Pluggable scene subsystems, run by tick() in phase order (see
        // EditorSceneSystem.hpp). Replaces the hardcoded `if (member_)` god-tick;
        // each subsystem (camera, anim resolver, selection, world streaming, …) is
        // registered in bringUp and owns its own long-lived state. A scene that
        // doesn't want a capability simply omits its registration. Cleared in
        // tearDown so the next bringUp starts fresh.
        std::vector<std::unique_ptr<EditorSceneSystem>> systems_;

        // Non-owning back-pointer into systems_ for the one cross-cutting query the
        // host still needs from EditorScene: cameraWantsCursorCapture() forwards here.
        // Set when the CameraSceneSystem is registered (bringUp); nulled in tearDown.
        CameraSceneSystem*                  camera_system_{nullptr};

        // Mirrored from `infra_` for fast access — the render-side scene / view live
        // for the editor process's lifetime; tearDown does NOT destroy them. (All
        // feature op-ids — camera, skinning, light, sky, grid, line, tonemap — are
        // NOT mirrored here; consumers look them up BY NAME from
        // infra_->feature_registry, e.g. ops<ViewCameraOperationIds>("StandardViewCamera").)
        lux::render::RenderSceneId          scene_id_{};
        lux::render::ViewHandle             main_view_{};

        // Resize coalescing — set by callback, consumed by processPendingResize.
        uint32_t pending_resize_w_{0};
        uint32_t pending_resize_h_{0};
        bool     pending_resize_{false};

        // ECS-driven editor camera.
        lux::meta::entity_id                camera_entity_{lux::meta::null_entity};
        OrbitCameraState                    orbit_{};
        float                               elapsed_{0.f};

        // Shared editor selection state (ensured from the StateRegistry; held by
        // shared_ptr so it lives while in use). onPick writes it; F-focus + the
        // gizmo (both now in scene systems) read it via the tick context. Single
        // source of truth.
        std::shared_ptr<Selection>          sel_;

        // M2: per-mesh-asset local-space AABB cache. The serializer does
        // NOT pre-compute `Mesh::bounds` today, so the first picking call
        // that references a mesh scans its vertex positions; subsequent
        // picks hit the cache. Cleared on `tearDown`.
        std::unordered_map<lux::asset::asset_id_t, lux::math::AABB>
                                            mesh_aabb_cache_;

        bool                                live_{false};
    };

} // namespace lux::editor
