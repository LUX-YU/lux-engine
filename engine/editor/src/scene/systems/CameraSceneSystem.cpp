#include <lux/engine/editor/scene/systems/CameraSceneSystem.hpp>

#include <lux/engine/ecs/render/components/ViewPresentComponent.hpp>   // 视口尺寸从世界里查

#include <lux/engine/editor/app/EditorActions.hpp>  // actions:: ActionIds
#include <lux/engine/editor/app/Selection.hpp>      // Selection::entity (3D focal provider)

#include <lux/engine/editor/scene/controllers/EditorCamera3DController.hpp>
#include <lux/engine/editor/scene/controllers/EditorCamera2DController.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/SpatialTransformMath.hpp>

namespace lux::editor
{
    // ── Per-kind navigator adapters ─────────────────────────────────────────
    //  Each wraps its concrete controller + the action-id wiring / aspect fit
    //  its old wrapper system carried. Local to this TU: the rest of the
    //  editor sees only IEditorCameraController.
    namespace
    {
        class Camera3DNavigator final : public IEditorCameraController
        {
        public:
            Camera3DNavigator(Selection* selection, lux::meta::entity_id camera) noexcept
                : camera_(camera), selection_(selection) {}

            void attach(lux::meta::EntityRegistry& reg) override
            {
                controller_ = std::make_unique<EditorCamera3DController>();
                controller_->attach(camera_, reg);

                EditorCamera3DController::ActionIds ids;
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

                // Focal point for F-focus / orbit. The editor's shared
                // Selection is HOST state (no longer in the shared tick
                // context) — injected at construction, stable for the scene's
                // lifetime.
                Selection* sel = selection_;
                controller_->setSelectionProvider(
                    [sel] { return sel ? sel->entity() : lux::meta::null_entity; });
            }

            void tick(lux::meta::EntityRegistry& reg,
                      const lux::input::ActionMapper& mapper, float dt,
                      float content_w, float content_h) override
            {
                controller_->tick(mapper, dt);
                // Camera3DSystem owns projection derivation, including reading
                // ViewPresentComponent::extent for auto-aspect.
                (void)content_w;
                (void)content_h;
            }

            [[nodiscard]] bool wantsCursorCapture() const noexcept override
            {
                return controller_ && controller_->wantsCursorCapture();
            }

            [[nodiscard]] Eigen::Vector3f eye(lux::meta::EntityRegistry& reg) const override
            {
                const auto* wt = reg.try_get<lux::ecs::ResolvedTransform3DComponent>(camera_);
                if (!wt)
                    return Eigen::Vector3f::Zero();
                return lux::ecs::relativePosition(
                    wt->position,
                    {},
                    lux::ecs::kDefaultRelativeSpatialExtent
                ).value_or(Eigen::Vector3f::Zero());
            }

            [[nodiscard]] std::optional<lux::math::Position3d>
            worldFocus3D(lux::meta::EntityRegistry&) const override
            {
                return controller_
                    ? controller_->orbitTargetWorld()
                    : std::nullopt;
            }

        private:
            std::unique_ptr<EditorCamera3DController> controller_;
            /// 编辑器脚手架相机。批 3 之前从 ctx.camera_entity 读 —— 运行时不再
            /// 镜像「谁在出图」，这是宿主知识，装配期注入。
            lux::meta::entity_id                      camera_{lux::meta::null_entity};
            Selection*                                selection_{nullptr};
        };

        class Camera2DNavigator final : public IEditorCameraController
        {
        public:
            explicit Camera2DNavigator(lux::meta::entity_id camera) noexcept
                : camera_(camera) {}

            void attach(lux::meta::EntityRegistry& reg) override
            {
                controller_ = std::make_unique<EditorCamera2DController>();
                controller_->attach(camera_, &reg);

                EditorCamera2DController::ActionIds ids;
                ids.pan_mode    = actions::CameraPanMode;      // shared chord with 3D pan
                ids.look_x      = actions::CameraLookYaw;      // raw mouse delta X
                ids.look_y      = actions::CameraLookPitch;    // raw mouse delta Y
                ids.zoom_scroll = actions::CameraSpeedScroll;  // wheel = zoom in 2D
                ids.reset       = actions::CameraReset;
                controller_->setActionIds(ids);
            }

            void tick(lux::meta::EntityRegistry& reg,
                      const lux::input::ActionMapper& mapper, float dt,
                      float content_w, float content_h) override
            {
                controller_->tick(mapper, dt, content_h);
                // Camera2DSystem owns aspect derivation from presentation data.
                (void)content_w;
            }
            // wantsCursorCapture / eye: interface defaults (false / zero).

        private:
            std::unique_ptr<EditorCamera2DController> controller_;
            lux::meta::entity_id                      camera_{lux::meta::null_entity};
        };
    } // namespace

    std::unique_ptr<IEditorCameraController>
    makeEditorCamera3DNavigator(Selection* selection, lux::meta::entity_id camera)
    { return std::make_unique<Camera3DNavigator>(selection, camera); }
    std::unique_ptr<IEditorCameraController>
    makeEditorCamera2DNavigator(lux::meta::entity_id camera)
    { return std::make_unique<Camera2DNavigator>(camera); }

    // ── The one camera system ────────────────────────────────────────────────
    CameraSceneSystem::CameraSceneSystem(std::unique_ptr<IEditorCameraController> navigator,
                                         lux::meta::entity_id                     camera,
                                         lux::ecs::SystemType                     camera_system)
        : nav_(std::move(navigator)),
          camera_(camera),
          camera_system_(camera_system) {}
    CameraSceneSystem::~CameraSceneSystem() = default;

    void CameraSceneSystem::update(const lux::ecs::SystemUpdateContext& ctx)
    {
        auto& reg = ctx.registry();
        const float dt = ctx.dt();
        if (!nav_ || !mapper_)
            return;
        // Self-guard: the runtime no longer aborts the whole tick on a broken
        // camera (the world must keep simulating — SceneRuntime::tick), so THIS
        // system checks its own precondition. A vanished scaffolding camera (a
        // play script destroyed it) skips camera navigation; everything else runs.
        {
            if (camera_ == lux::meta::null_entity ||
                !reg.valid(static_cast<entt::entity>(camera_)) ||
                !reg.any_of<lux::ecs::Camera3DComponent,
                            lux::ecs::Camera2DComponent>(
                    static_cast<entt::entity>(camera_)))
                return;
        }
        // Lazy attach on the first valid tick: attach() reads the home pose,
        // so the camera entity must be live with its composed components (the
        // editor binds its scaffolding camera during bringUp, before the
        // first tick).
        if (!attached_)
        {
            nav_->attach(reg);
            attached_ = true;
        }
        nav_->tick(reg, *mapper_, dt, content_w_, content_h_);
    }

    bool CameraSceneSystem::wantsCursorCapture() const noexcept
    {
        return nav_ && attached_ && nav_->wantsCursorCapture();
    }

    Eigen::Vector3f CameraSceneSystem::eye(lux::meta::EntityRegistry& reg) const
    {
        return nav_ ? nav_->eye(reg) : Eigen::Vector3f::Zero();
    }

    std::optional<lux::math::Position3d>
    CameraSceneSystem::worldFocus3D(lux::meta::EntityRegistry& reg) const
    {
        return nav_ && attached_ ? nav_->worldFocus3D(reg) : std::nullopt;
    }

} // namespace lux::editor
