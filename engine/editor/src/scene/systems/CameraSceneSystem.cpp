#include <lux/engine/editor/scene/systems/CameraSceneSystem.hpp>

#include <lux/engine/editor/app/LuxEditor.hpp>      // EditorRenderInfra (feature_registry)
#include <lux/engine/editor/app/EditorActions.hpp>  // actions:: ActionIds
#include <lux/engine/editor/app/Selection.hpp>      // Selection::entity (focal provider)

#include <lux/pack/d3/controllers/UEEditorController.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/pack/d3/world/components/CameraComponent.hpp>

#include <lux/engine/ui/UIRenderSession.hpp>                // ctx.session (RenderSession base)
#include <lux/engine/render/comm/client/RenderSession.hpp>  // ViewCameraProxy upcast target

namespace lux::editor
{
    // Out-of-line so the fwd-declared UEEditorController is complete here.
    CameraSceneSystem::CameraSceneSystem()  = default;
    CameraSceneSystem::~CameraSceneSystem() = default;

    // -------------------------------------------------------------------------
    void CameraSceneSystem::onPreWorldTick(const SceneTickContext& ctx)
    {
        // Lazy-build the controller on the first valid tick: attach() reads the home
        // pose, so the camera entity must be live AND carry a TransformComponent
        // (EditorScene::tick's camera-validity guard guarantees both before we run).
        if (!controller_)
        {
            controller_ = std::make_unique<lux::pack::UEEditorController>();
            controller_->attach(ctx.camera_entity, ctx.world);

            lux::pack::UEEditorController::ActionIds ids;
            ids.fly_mode       = actions::CameraFlyMode;
            ids.orbit_mode     = actions::CameraOrbitMode;
            ids.pan_mode       = actions::CameraPanMode;
            ids.alt_modifier   = actions::CameraAltModifier;
            ids.move_forward   = actions::CameraMoveForward;
            ids.move_right     = actions::CameraMoveRight;
            ids.move_up        = actions::CameraMoveUp;
            ids.look_yaw       = actions::CameraLookYaw;
            ids.look_pitch     = actions::CameraLookPitch;
            ids.speed_scroll   = actions::CameraSpeedScroll;
            ids.focus_selected = actions::CameraFocusSelected;
            ids.reset          = actions::CameraReset;
            controller_->setActionIds(ids);

            // Focal point for F-focus / orbit. ctx.selection is the editor's single
            // shared Selection — stable for the scene's lifetime, captured by value.
            Selection* sel = ctx.selection;
            controller_->setSelectionProvider(
                [sel] { return sel ? sel->entity() : lux::meta::null_entity; });
        }

        controller_->tick(ctx.mapper, ctx.dt);

        // Auto-fit the projection aspect to the viewport content size, unless the
        // user took manual control via the Inspector (auto_aspect gate — otherwise
        // any aspect edit is clobbered next frame). Must precede World::tick, which
        // recomputes proj from aspect.
        auto& cc = ctx.world.get<lux::pack::CameraComponent>(ctx.camera_entity);
        if (cc.auto_aspect && ctx.content_h > 0.f)
            cc.aspect = ctx.content_w / ctx.content_h;
    }

    // -------------------------------------------------------------------------
    void CameraSceneSystem::onPostRenderableUpdate(const SceneTickContext& ctx)
    {
        // Camera is a feature now (StandardViewCamera) — resolve its dynamic op-ids
        // BY NAME once. Absent feature → invalid ops → ViewCameraProxy is a graceful
        // no-op.
        if (!view_ops_resolved_)
        {
            view_ops_ = ctx.infra.feature_registry
                            .ops<lux::render::ViewCameraOperationIds>("StandardViewCamera");
            view_ops_resolved_ = true;
        }

        const auto& cc = ctx.world.get<lux::pack::CameraComponent>(ctx.camera_entity);
        lux::render::ViewCameraProxy(ctx.session, view_ops_).update(
            ctx.scene_id, ctx.main_view,
            cc.view.data(), cc.proj.data(), ctx.eye.data());
    }

    // -------------------------------------------------------------------------
    bool CameraSceneSystem::wantsCursorCapture() const noexcept
    {
        return controller_ && controller_->wantsCursorCapture();
    }

} // namespace lux::editor
