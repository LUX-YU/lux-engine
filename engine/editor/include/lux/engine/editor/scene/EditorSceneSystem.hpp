#pragma once
/**
 * @file EditorSceneSystem.hpp
 * @brief Editor-side scene-subsystem extension point (de-hardcodes EditorScene::tick).
 *
 * EditorScene::tick used to hardcode a fixed sequence of subsystems inline
 * (camera controller, anim resolver, world streaming, selection publish,
 * renderable bridge, CPU-data eviction, camera push, selection gizmo), each a
 * named member guarded by `if (member_)`. Adding/removing a scene capability
 * meant editing that god method + bringUp + the header member list — an OCP
 * violation, and the lone holdout vs the engine's own register-don't-hardcode
 * convention (gameplay World::addSystem, RenderableSystem adapters, RenderFeature
 * registry, ComponentTypeRegistry, ThumbnailSpecProviders).
 *
 * An `EditorSceneSystem` is a registered, pluggable scene subsystem. A scene that
 * doesn't want streaming simply doesn't register the streaming system. The shape
 * mirrors RenderFeature (register + ordered phase callbacks + isEnabled); each
 * system captures its OWN long-lived state, and reads per-frame host services
 * from the SceneTickContext passed to each phase (the RenderableSystem ctor-
 * capture pattern, generalized).
 *
 * Why THREE phases (not one update): the tick has two load-bearing anchors —
 * `World::tick` (CameraSystem/AnimationSystem run here) and
 * `RenderableSystem::update` (entt→GPU bridge: reads dormant tags + highlight,
 * reaps instances). Streaming must set dormant tags BEFORE the bridge and evict
 * CPU data AFTER it. So phases bracket the two anchors:
 *   onPreWorldTick        — before World::tick      (camera input, anim resolve)
 *   onPreRenderableUpdate  — after World::tick, before the bridge (streaming, selection-highlight)
 *   onPostRenderableUpdate — after the bridge        (eviction, camera push, gizmo)
 * The two anchors stay EditorScene-owned and are exposed via the context, so a
 * single ordered system list expresses the whole frame without a god method.
 */

#include <Eigen/Core>

#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/core/FeatureHandle.hpp>   // ViewHandle
#include <lux/engine/meta/LuxObject.hpp>              // entity_id

namespace lux::gameplay { class World; }
namespace lux::input { class ActionMapper; }
namespace lux::gameplay { class RenderableSystem; }
namespace lux::ui       { class UIRenderSession; }
namespace lux::asset    { class AssetManager; }

namespace lux::editor
{
    struct EditorRenderInfra;
    class Selection;

    /// Per-frame host + scene services handed to each EditorSceneSystem phase.
    /// References are valid only for the duration of the tick. `eye` (camera world
    /// position) is only valid in the pre/post-Renderable phases (it is read AFTER
    /// World::tick updates the camera's WorldTransform).
    struct SceneTickContext
    {
        lux::gameplay::World&               world;          ///< the ECS world (.registry())
        lux::gameplay::RenderableSystem&    renderable;     ///< entt→GPU bridge anchor (shared)
        lux::ui::UIRenderSession&           session;        ///< render session for proxies
        const EditorRenderInfra&            infra;          ///< scene_id / view / feature_registry
        lux::asset::AssetManager*           assets;         ///< may be null
        const lux::input::ActionMapper&  mapper;         ///< input this frame
        Selection*                          selection;      ///< shared editor selection (may be null)

        lux::render::RenderSceneId          scene_id{};
        lux::render::ViewHandle             main_view{};
        lux::meta::entity_id                camera_entity{};

        float                               dt{0.f};
        float                               content_w{0.f};
        float                               content_h{0.f};
        Eigen::Vector3f                     eye{Eigen::Vector3f::Zero()};  ///< valid in (pre/post)-Renderable phases
    };

    /// A pluggable editor scene subsystem. Default phases are no-ops; a system
    /// overrides only the phase(s) it needs. Register via EditorScene in bringUp.
    class EditorSceneSystem
    {
    public:
        virtual ~EditorSceneSystem() = default;

        virtual void onPreWorldTick(const SceneTickContext&) {}
        virtual void onPreRenderableUpdate(const SceneTickContext&) {}
        virtual void onPostRenderableUpdate(const SceneTickContext&) {}

        /// Skipped this frame when false (per-system gate, like RenderFeature::isEnabled).
        [[nodiscard]] virtual bool isEnabled() const { return true; }
    };

} // namespace lux::editor
