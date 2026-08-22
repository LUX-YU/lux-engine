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
 *   - the `lux::ecs::RenderSystem` that bridges the World's
 *     transform/mesh components into the renderer
 *
 * It does NOT own the `UIRenderFrameSession` or the `AssetManager` — those are
 * process-wide and survive scene swaps; references are stored.
 *
 * Lifecycle:
 *
 *   1. construct (cheap, no GPU work)
 *   2. `bringUp(cfg)` — synchronous; issues create/register/add
 *      commands and pumps frames until each reply lands
 *   3. each frame: `processPendingResize(session)` → `tick(dt, w, h)`
 *   4. `tearDown()` — explicit; issues async removeFeature /
 *      destroyRenderTarget / removeView / destroyScene and pumps once so the render thread
 *      drains them
 *   5. destruct (must be a no-op; async render teardown cannot complete
 *      from a destructor)
 *
 * Scene swapping rides exactly this contract — `SceneController::loadScene`
 * tears down the current `EditorScene`, replaces the `unique_ptr`, then brings
 * up the new one.
 */

#include <lux/engine/editor/visibility.h>

#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/math/AABB.hpp>
#include <lux/engine/ecs/Registry.hpp>   // entity_id, null_entity
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/RenderLease.hpp>
#include <lux/engine/function/render/client/genops/SkinningOperation.ops.hpp>  // SkinningOperationIds
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>        // LightOperationIds
#include <lux/engine/function/render/client/FeatureCatalog.hpp>   // per-scene features_ (C2)
#include <lux/engine/ecs/render/components/3d/SceneSettingsComponent.hpp>  // ensureSceneSettings return type
#include <lux/engine/ecs/render/components/ViewPresentComponent.hpp>                  // viewport_slot_（按值持有）
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DSystem.hpp>
#include <lux/engine/runtime/render/scene/RenderSceneIntegration.hpp>
#include <lux/engine/runtime/scene/script/SceneScriptRuntime.hpp>
#include <lux/engine/navigation/Navigation.hpp>
#include <lux/engine/ecs/SystemPhase.hpp>          // 相位常量(宿主注册自己的系统时用)
#include <lux/engine/editor/app/Selection.hpp>            // scene-domain Selection member (C11)
#include <lux/engine/editor/scene/InstanceSpawnClient.hpp>
#include <lux/engine/authoring/world/WorldSource.hpp>
#include <lux/engine/authoring/world/WorldDescriptorIndex.hpp>
#include <lux/engine/authoring/world/WorldAuthoringTransaction.hpp>
#include <lux/engine/authoring/world/WorldTerrainAuthoring.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lux::asset    { class AssetManager; class AssetVfs; }
namespace lux::ecs { class ComponentTypeCatalog; class World; }
namespace lux::input    { class ActionMapper; class InputActionRegistry; }
namespace lux::ui       { class UIRenderFrameSession; class SceneViewportPanel; }
namespace lux::exec     { class AsyncRuntime; }

namespace lux::editor
{
    struct EditorRenderInfra;
    class CameraSceneSystem;

    struct WorldTerrainEditStats final
    {
        std::size_t loaded_pages{0u};
        std::size_t dirty_pages{0u};
        std::size_t undo_depth{0u};
        std::size_t redo_depth{0u};
        bool load_pending{false};
        bool heightmap_io_pending{false};
        std::string last_error;
    };

    struct WorldAuthoringResidencyStats final
    {
        std::size_t descriptor_pages{0u};
        std::size_t descriptor_bytes{0u};
        std::size_t instance_pages{0u};
        std::size_t instances{0u};
        std::size_t dirty_instance_pages{0u};
        std::size_t pending_descriptor_pages{0u};
        std::size_t pending_instance_pages{0u};
    };

    struct BringUpConfig
    {
        std::string name           = "EditorScene";
        uint32_t    initial_width  = 1280;
        uint32_t    initial_height = 720;
        /// Project-local cache for immutable Editor Play Cook products. Empty
        /// uses a process temp fallback; Authoring source is never rewritten
        /// merely to enter Play.
        std::filesystem::path play_cache_root{};

        /// Optional path to a `.luxworld` file. If non-empty and the file
        /// exists, `bringUp` loads the entity tree from it via
        /// the EntityScene publication path after the World + RenderSystem
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
    /// (fov / near / far / aspect) on its `Camera3DComponent`; this
    /// struct only describes "where the camera is around the target."
    ///
    /// Seeded once at bringUp (via writeOrbitToTransform) to place the camera;
    /// thereafter `CameraSceneSystem`'s `EditorCamera3DController` drives the camera
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

    class EditorAsyncService;
    struct EntityScenePlayCookJob;
    struct CookEntitySceneValue;

    class LUX_EDITOR_PUBLIC EditorScene
    {
    public:
        /// Asset loading is a typed process-domain service. This scene keeps
        /// only its narrow client and the runtime that owns structured scopes.
        EditorScene(lux::ui::UIRenderFrameSession& session,
                    lux::asset::AssetManager& assets,
                    const EditorRenderInfra&  infra,
                    lux::asset_runtime::AssetClient asset_client,
                    lux::exec::AsyncRuntime& async,
                    EditorAsyncService& editor_async,
                    const lux::ecs::ComponentTypeCatalog& components) noexcept;

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
        [[nodiscard]] bool tearDown() noexcept;

        // ── Per-frame ──────────────────────────────────────────────────

        /// Queued by the SceneViewportPanel's resize callback (which fires
        /// inside `paint()`); applied by `processPendingResize` once the
        /// next frame is open.
        void queueResize(uint32_t width, uint32_t height) noexcept;

        /// Flush a pending viewport resize via `session.resizeTarget`. No-op
        /// if no resize is queued.
        void processPendingResize();

        /// Advance the world by `dt`, drive the UE-style camera controller
        /// from the editor-wide `ActionMapper`, refresh the aspect ratio
        /// from `content_w / content_h`, push the camera to the renderer,
        /// then drive the RenderSystem to bridge mesh updates.
        ///
        /// @param mapper Per-frame action snapshot from `LuxEditor::run`.
        ///               Must outlive this call but not across calls; the
        ///               controller does not retain it.
        void tick(float dt, float content_w, float content_h,
                  const lux::input::ActionMapper& mapper);

        // ── Selection — drives F-focus + viewport picking ───────────────
        //
        // The Selection is SCENE-DOMAIN state: it points into
        // this scene's World, so this scene owns it — born bound to the World
        // at bringUp, cleared at tearDown, gone with the object. Panels hold a
        // raw pointer refreshed through SceneController::setOnSceneChanged
        // (null between scenes); scene systems get it via the tick context.

        /// Resolve a screen-space click to an entity and write this scene's
        /// `Selection`. Coordinates are content-rect-
        /// local pixels, top-left = (0,0); @p cw / @p ch are the content size;
        /// the projection is read from the active Camera3DComponent. Sets the
        /// hit entity (or null on miss). No-op when not live.
        void onPick(float cx, float cy, float cw, float ch);

        /// Convert a content-rect-local viewport point to the 2D world position
        /// under it (ortho screen→world through the editor camera's Camera2D
        /// cache — the same math the 2D pick uses). Empty when the scene is not
        /// live / not a 2D-camera scene. Used by "create HERE" (viewport
        /// right-click → spawn at the tap).
        [[nodiscard]] std::optional<lux::math::Position2d>
            viewportToWorld2D(float cx, float cy, float cw, float ch) const;
        [[nodiscard]] std::optional<lux::math::Position3d>
            viewportFocus3D() const;

        /// True only while the camera is in fly mode (RMB held in the
        /// viewport). LuxEditor reads this each frame to flip the GLFW
        /// cursor between NORMAL and DISABLED.
        [[nodiscard]] bool cameraWantsCursorCapture() const noexcept;

        // ── Edit/Play — Cooked Parity ─────────────────────────────────────

        /// Capture immutable Authoring documents, enqueue a deterministic
        /// Spatial3D EntityScene Cook, then start an independent SceneRuntime
        /// from LXSC/LXES. The edit Registry is never used as the simulated
        /// World. @p mapper has an editor-stable address; @p actions may be
        /// null.
        [[nodiscard]] bool enterPlay(const lux::input::ActionMapper&        mapper,
                                     const lux::input::InputActionRegistry* actions);

        /// Cancel an in-flight Cook or close the cooked Play Runtime, then
        /// restore the edit Runtime's viewport camera. No-op when already idle.
        void exitPlay();

        /// True between enterPlay and exitPlay.
        [[nodiscard]] bool isPlaying() const noexcept
        { return play_cook_pending_ || play_runtime_ != nullptr; }

        // ── Entity authoring — scene-domain actions ────────────

        /// Asynchronously load/decode every model dependency, wait for mesh and
        /// material GPU residency without waiting for a render frame, then
        /// create the ECS tree at a main-thread safe point. The completion is
        /// exactly once and carries a structured failure.
        [[nodiscard]] lux::async::SubmitResult spawnModel(
            lux::asset::asset_id_t model_id,
            InstanceSpawnClient::Completion completion = {});

        // ── File binding — the .luxworld this edit session saves to ──
        //
        // Scene-domain state (moved in from SceneController):
        // bound at bringUp from BringUpConfig::from_scene_file, re-bound by a
        // successful saveTo. Empty = a new scene never saved anywhere yet.

        [[nodiscard]] const std::filesystem::path& scenePath() const noexcept
        { return scene_path_; }

        /// Serialize this scene's World to @p file (active camera + assembly
        /// plan ride in the header) and re-bind scenePath() on success.
        [[nodiscard]] bool saveTo(const std::filesystem::path& file);

        /// saveTo(scenePath()); false when the scene has no bound file yet
        /// (the caller then routes through a Save-As dialog).
        [[nodiscard]] bool save();

        // ── Accessors ──────────────────────────────────────────────────

        lux::ecs::World&        world()       noexcept { return runtime_->world(); }
        const lux::ecs::World&  world() const noexcept { return runtime_->world(); }

        /// This scene's Selection (scene-domain): born bound to
        /// the World at bringUp, cleared at tearDown. Pointers handed out live
        /// exactly as long as this scene — consumers re-target on scene change.
        [[nodiscard]] Selection&       selection()       noexcept { return selection_; }
        [[nodiscard]] const Selection& selection() const noexcept { return selection_; }

        [[nodiscard]] std::size_t indexedWorldActorCount() const noexcept;
        [[nodiscard]] std::size_t materializedWorldActorCount() const noexcept
        {
            return materialized_actor_ids_.size();
        }
        [[nodiscard]] WorldAuthoringResidencyStats
            worldAuthoringResidencyStats() const noexcept
        {
            return {
                world_descriptor_pages_.size(),
                descriptor_page_resident_bytes_,
                authoring_instance_clusters_.size(),
                authoring_instance_resident_count_,
                dirty_instance_pages_.size(),
                pending_viewport_descriptor_pages_.size(),
                pending_instance_pages_.size()};
        }
        [[nodiscard]] lux::authoring::WorldSourceDocument*
        worldSource() noexcept
        {
            return world_source_.get();
        }

        [[nodiscard]] const lux::authoring::WorldSourceDocument*
        worldSource() const noexcept
        {
            return world_source_.get();
        }
        [[nodiscard]] std::optional<
            lux::runtime::spatial3d::Physics3DSceneSnapshot>
            physics3DDebugSnapshot() noexcept;
        [[nodiscard]] std::optional<
            lux::navigation::NavigationPathResult>
        queryNavigationPath(
            const lux::navigation::NavigationPathRequest& request) noexcept;
        [[nodiscard]] std::vector<
            lux::authoring::WorldDescriptorIndexActor> queryWorldActors(
                std::string_view text,
                std::size_t offset,
                std::size_t maximum) const;
        [[nodiscard]] bool requestWorldActorProxy(
            lux::authoring::WorldActorId actor,
            bool select = true);

        [[nodiscard]] std::optional<lux::authoring::EditableWorldInstance>
            worldInstance(lux::authoring::WorldInstanceId instance) const;
        [[nodiscard]] lux::cxx::expected<void, std::string>
            updateWorldInstance(
                lux::authoring::EditableWorldInstance instance);
        [[nodiscard]] lux::cxx::expected<
            lux::authoring::WorldInstanceId,
            std::string>
            duplicateWorldInstance(lux::authoring::WorldInstanceId instance);
        [[nodiscard]] lux::cxx::expected<void, std::string>
            deleteWorldInstance(lux::authoring::WorldInstanceId instance);
        [[nodiscard]] lux::cxx::expected<
            lux::authoring::WorldActorId,
            std::string>
            convertWorldInstanceToActor(
                lux::authoring::WorldInstanceId instance);
        [[nodiscard]] lux::cxx::expected<
            lux::authoring::WorldInstanceId,
            std::string>
            convertWorldActorToInstance(
                lux::authoring::WorldActorId actor);
        [[nodiscard]] const std::string& worldInstanceEditError() const noexcept
        {
            return instance_edit_error_;
        }
        [[nodiscard]] const std::string&
            worldInstancePreviewStatus() const noexcept
        {
            return instance_preview_status_;
        }

        [[nodiscard]] bool requestWorldTerrainRegion(
            lux::authoring::TerrainSetId terrain,
            lux::authoring::PartitionSpaceId space,
            std::vector<lux::authoring::WorldCellKey> cells);
        [[nodiscard]] bool applyWorldTerrainBrush(
            lux::authoring::TerrainSetId terrain,
            std::span<const lux::authoring::WorldCellKey> cells,
            const lux::math::Position3d& center,
            const lux::authoring::WorldTerrainBrush& brush);
        [[nodiscard]] bool undoWorldTerrainEdit();
        [[nodiscard]] bool redoWorldTerrainEdit();
        [[nodiscard]] lux::cxx::expected<
            lux::authoring::WorldTerrainHeightmap16,
            lux::authoring::WorldTerrainAuthoringFailure>
        exportWorldTerrainHeightmap16(
            lux::authoring::TerrainSetId terrain,
            std::span<const lux::authoring::WorldCellKey> cells) const;
        [[nodiscard]] bool importWorldTerrainHeightmap16(
            lux::authoring::TerrainSetId terrain,
            std::span<const lux::authoring::WorldCellKey> cells,
            const lux::authoring::WorldTerrainHeightmap16& image);
        [[nodiscard]] bool requestImportWorldTerrainHeightmap16(
            lux::authoring::TerrainSetId terrain,
            std::vector<lux::authoring::WorldCellKey> cells,
            std::filesystem::path raw16_file);
        [[nodiscard]] bool requestExportWorldTerrainHeightmap16(
            lux::authoring::TerrainSetId terrain,
            std::vector<lux::authoring::WorldCellKey> cells,
            std::filesystem::path raw16_file);
        [[nodiscard]] WorldTerrainEditStats worldTerrainEditStats() const;
        [[nodiscard]] std::optional<lux::authoring::PartitionSpaceId>
            defaultWorldTerrainSpace() const;
        [[nodiscard]] std::optional<lux::math::Position3d>
            makeWorldTerrainPosition(
                lux::authoring::PartitionSpaceId space,
                const lux::authoring::WorldCellKey& cell,
                float local_x,
                float local_z) const;

        lux::render::RenderSceneId sceneId() const noexcept
        {
            const auto* active = play_runtime_ ? play_runtime_.get()
                                               : runtime_.get();
            const auto* render = active
                ? lux::runtime::renderScene(*active)
                : nullptr;
            return render ? render->sceneId() : lux::render::RenderSceneId{};
        }
        // mainView() 删除(批 3):零调用点,而且「主视图」不再是运行时的一个字段 ——
        // view 归相机所有,要查就查 `view<ViewPresentComponent, RenderViewBindingComponent>`。
        /// 主视图的显式 SAMPLED 渲染目标(视口面板经 target sentinel 采样)。
        lux::render::RenderTargetId  mainTarget() const noexcept { return main_target_.id(); }

        // (曾有 features() 转发:场景级的特性目录副本。目录如今是进程域的
        //  一份,面板经 LuxEditor::featureCatalog() 直取;每场景句柄住在
        //  RenderSystem 的绑定表 —— 裁决二,副本消亡。)

        OrbitCameraState&            orbit()        noexcept { return orbit_; }

        /// True only between a successful `bringUp` and a `tearDown`.
        [[nodiscard]] bool isLive() const noexcept { return live_; }

        /// Capability selection comes only from the LXSC/LXWA contribution
        /// list. There is no top-level 2D/3D scene discriminator.
        [[nodiscard]] bool hasContribution(
            std::string_view contribution) const noexcept;

        /// FQNs of component schemas installed in this editor process. Scene
        /// contributions assemble behavior and services; they are deliberately
        /// not a second component-type catalog. Recomputed on demand (only the
        /// Add-Component menu asks).
        [[nodiscard]] std::vector<std::string> availableComponentFqns() const
        {
            std::vector<std::string> result;
            result.reserve(components_.all().size());
            for (const auto& component : components_.all())
                result.emplace_back(component.fullName());
            return result;
        }

        /// The scene-global Render View settings singleton. Spatial selection
        /// policy belongs to the installed scene contributions/components.
        /// Main-thread only (mutates the registry). Precondition: scene is live.
        lux::ecs::SceneSettingsComponent& ensureSceneSettings();

    private:
        [[nodiscard]] CameraSceneSystem* cameraSystem() noexcept;
        [[nodiscard]] const CameraSceneSystem* cameraSystem() const noexcept;
        // Borrowed, never null: all three outlive the scene by construction
        // (LuxEditor::init builds them before any scene exists and drops them
        // after the last one is gone). References, so that stays checkable by
        // the compiler rather than by a guard at every use site.
        lux::ui::UIRenderFrameSession&           session_;
        lux::asset::AssetManager&           assets_;
        const EditorRenderInfra&            infra_;
        lux::asset_runtime::AssetClient      asset_client_;
        lux::exec::AsyncRuntime&              async_;
        EditorAsyncService&                   editor_async_;
        const lux::ecs::ComponentTypeCatalog& components_;

        /// The host-neutral scene runtime: render-scene orchestration, World
        /// assembly, bridges, scene load, the three-phase tick and the
        /// simulation lifecycle all live THERE now — this class is its editor
        /// HOST (offscreen target, scaffolding camera, selection/pick,
        /// Authoring session, panels). Constructed in bringUp, destroyed in
        /// tearDown. Cooked Play owns a separate runtime below.
        std::unique_ptr<lux::runtime::SceneRuntime> runtime_;

        /// Scene-scoped async intent owner. Declared after runtime_ so passive
        /// destruction closes it before the World; tearDown closes it actively
        /// while AsyncRuntime/ResidencyAssembly are still available.
        std::unique_ptr<InstanceSpawnClient> instance_spawn_;
        std::vector<lux::asset::AssetRef> spawn_handoff_pins_;

        // Generation-checked observation into the runtime-owned Schedule.
        lux::ecs::SystemHandle<CameraSceneSystem> camera_system_{};

        [[nodiscard]] lux::ecs::Entity commitSpawnModel(InstanceSpawnPlan&& plan);
        [[nodiscard]] std::shared_ptr<const EntityScenePlayCookJob>
            buildEntityScenePlayCookJob(
                const std::filesystem::path& root_document);
        void adoptCookedPlay(
            CookEntitySceneValue value,
            std::uint64_t generation) noexcept;
        [[nodiscard]] bool closeCookedPlay() noexcept;
        void updateAuthoringProxyWindow();
        void requestAuthoringDescriptorPage(const lux::authoring::WorldDescriptorPageReference& page);
        void requestAuthoringInstancePage(
            uuids::uuid descriptor_page,
            const lux::authoring::WorldPageSourceDescriptor& page);
        [[nodiscard]] lux::authoring::WorldDescriptorPageDocument*
        cachedAuthoringDescriptorPage(uuids::uuid page) noexcept;
        void cacheAuthoringDescriptorPage(lux::authoring::WorldDescriptorPageDocument page);
        void trimAuthoringDescriptorPageCache() noexcept;
        [[nodiscard]] bool activateAuthoringInstancePage(
            uuids::uuid descriptor_page,
            lux::authoring::WorldPageSourceDescriptor descriptor,
            lux::authoring::WorldInstancePageDocument page);
        void clearAuthoringInstanceClusters() noexcept;
        void restoreAuthoringViewpoint(
            lux::ecs::Entity source_camera) noexcept;

        // Pick strategy: bound once at bringUp next to
        // the camera navigator; onPick calls it kind-blind and applies the
        // shared root-promotion + selection epilogue.
        lux::ecs::Entity (EditorScene::*pick_fn_)(float, float, float, float){nullptr};
        lux::ecs::Entity pickImage2D(float cx, float cy, float cw, float ch);
        lux::ecs::Entity pickMesh3D(float cx, float cy, float cw, float ch);

        // The HOST-owned render target: bringUp creates the offscreen SAMPLED
        // target the viewport panel samples, hands its id to the runtime
        // (which composes the scene's view onto it) and tearDown destroys it
        // AFTER the runtime released the view/scene. Scene/view/features are
        // runtime-owned now.
        lux::render::RenderTargetLease      main_target_{};

        /// 视口那一路图的「出图槽位」（target / 层序 / 创建尺寸）。bringUp 记一次，
        /// Play/Stop 在两台相机之间搬的就是它。存成成员而不是每次从世界里捞：
        /// Play 且**没有**激活相机时，槽位被摘掉后世界里一份都不剩，Stop 就没有
        /// 可还原的东西了 —— 那正是「无激活相机 → 黑屏」这条路径的回程。
        lux::ecs::ViewPresentComponent      viewport_slot_{};

        // Resize coalescing — set by callback, consumed by processPendingResize.
        uint32_t pending_resize_w_{0};
        uint32_t pending_resize_h_{0};
        bool     pending_resize_{false};

        // ECS-driven editor camera.
        lux::ecs::Entity                camera_entity_{lux::ecs::kNullEntity};
        OrbitCameraState                    orbit_{};
        float                               elapsed_{0.f};

        // THIS scene's Selection (scene-domain, C11): value member — its
        // lifetime IS the scene's. onPick writes it; F-focus + the gizmo
        // (scene systems) read it via the tick context; panels re-target
        // their pointer on scene change. Single source of truth.
        Selection                           selection_;

        // File binding (C11, from SceneController): bound at bringUp, re-bound
        // by saveTo. Empty = never saved.
        std::filesystem::path               scene_path_;
        std::filesystem::path               play_cache_root_;

        /// Non-null only for the new LXWA Authoring path. Runtime never sees
        /// this object: Editor parses/materializes it into an explicit
        /// transient Runtime World, and Cook consumes the same root later.
        std::unique_ptr<lux::authoring::WorldSourceDocument> world_source_;
        std::unique_ptr<lux::authoring::WorldDescriptorIndex>
            world_descriptor_index_;
        struct CachedWorldDescriptorPage final
        {
            lux::authoring::WorldDescriptorPageDocument document;
            std::size_t resident_bytes{0u};
            std::uint64_t last_use{0u};
        };
        std::unordered_map<std::string, CachedWorldDescriptorPage>
                                            world_descriptor_pages_;
        std::unordered_map<
            std::string,
            lux::authoring::WorldActorSourceDescriptor>
                                            materialized_actor_descriptors_;
        std::size_t                         descriptor_page_resident_bytes_{0u};
        std::uint64_t                       descriptor_page_cache_clock_{0u};
        struct AuthoringEntityState final
        {
            std::vector<entt::entity> created_entities;
            std::vector<lux::authoring::WorldActorId> world_entity_ids;
            entt::entity primary_camera{entt::null};
        }                                   authoring_load_result_{};
        std::unordered_set<std::string>      pending_actor_proxies_;
        std::unordered_set<std::string>      pending_descriptor_pages_;
        std::unordered_set<std::string>
                                            pending_viewport_descriptor_pages_;
        std::unordered_set<std::string>      pending_instance_pages_;
        std::unordered_set<std::string>      materialized_actor_ids_;
        std::unordered_set<std::string>      dirty_actor_ids_;
        struct AuthoringInstanceCluster final
        {
            uuids::uuid descriptor_page{};
            lux::authoring::WorldPageSourceDescriptor descriptor;
            lux::authoring::WorldInstancePageDocument page;
        };
        std::unordered_map<std::string, AuthoringInstanceCluster>
                                            authoring_instance_clusters_;
        std::unordered_set<std::string>      desired_instance_pages_;
        std::unordered_set<std::string>      dirty_instance_pages_;
        std::string                         instance_edit_error_;
        std::string                         instance_preview_status_;
        std::size_t                         authoring_instance_resident_count_{0u};
        std::unordered_map<
            std::string,
            lux::authoring::WorldTerrainPageDocument> terrain_pages_;
        std::unordered_set<std::string>      dirty_terrain_pages_;
        std::vector<lux::authoring::WorldTerrainEditTransaction>
                                            terrain_undo_stack_;
        std::vector<lux::authoring::WorldTerrainEditTransaction>
                                            terrain_redo_stack_;
        std::string                         terrain_edit_error_;
        bool                                terrain_load_pending_{false};
        bool                                terrain_heightmap_io_pending_{false};
        float                                next_proxy_window_update_{0.0f};
        // M2: per-mesh-asset local-space AABB cache. The serializer does
        // NOT pre-compute `Mesh::bounds` today, so the first picking call
        // that references a mesh scans its vertex positions; subsequent
        // picks hit the cache. Cleared on `tearDown`.
        std::unordered_map<lux::asset::asset_id_t, lux::math::AABB>
                                            mesh_aabb_cache_;

        // ── Edit/Play state ──────────────────────────────────────────────
        struct PlayCookControl;
        std::shared_ptr<PlayCookControl>         play_cook_control_;
        std::shared_ptr<const lux::asset::AssetVfs> play_section_vfs_;
        std::unique_ptr<lux::runtime::SceneRuntime> play_runtime_;
        std::unique_ptr<lux::runtime::SceneScriptRuntime> play_simulation_;
        const lux::input::ActionMapper*          play_mapper_{nullptr};
        const lux::input::InputActionRegistry*   play_actions_{nullptr};
        std::uint64_t                            play_generation_{0u};
        bool                                     play_cook_pending_{false};

        bool                                live_{false};
    };

} // namespace lux::editor
