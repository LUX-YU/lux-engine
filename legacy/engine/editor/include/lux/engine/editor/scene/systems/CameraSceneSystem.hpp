#pragma once
/**
 * @file CameraSceneSystem.hpp
 * @brief Editor scene system: THE viewport camera — one system, per-kind
 *        NAVIGATOR injected at bringUp.
 *
 * The old shape was two near-identical wrapper systems (CameraSceneSystem /
 * Camera2DSceneSystem) around two navigator classes — the audit's "duplicated
 * logic" cluster. Now: ONE system owns an IEditorCameraController; which navigator it
 * carries is decided once from the selected spatial presentation contribution, and
 * the per-frame path never branches on kind again:
 *   onPreWorldTick — lazy attach() (home pose + action ids + selection
 *                    provider) then tick() (input → camera entity's transform
 *                    + aspect fit), before transform/camera schedule nodes.
 *   eye()          — the camera's world position after derived transforms (streaming
 *                    + context consumers); the 2D navigator reports zero.
 *   wantsCursorCapture() — forwarded by EditorScene::cameraWantsCursorCapture
 *                    (3D fly-mode captures the cursor; 2D never does).
 *
 * Camera → render-view upload is the BINDING model for both kinds
 * (RenderViewBindingComponent + Camera2D/3DUploadBridge) — no push phase here.
 */

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/math/Position.hpp>

#include <Eigen/Core>
#include <memory>
#include <optional>

namespace lux::input { class ActionMapper; }
namespace lux::ecs { class Camera2DSystem; class Camera3DSystem; }

namespace lux::editor
{
    class Selection;

    /// The per-kind viewport navigator contract. Implementations wrap the
    /// concrete controllers (EditorCamera3DController fly/orbit/pan,
    /// EditorCamera2DController pan/zoom) — defined in CameraSceneSystem.cpp,
    /// created via the factories below and injected at scene assembly.
    class IEditorCameraController
    {
    public:
        virtual ~IEditorCameraController() = default;

        /// One-time wiring on the first valid tick: home pose from the live
        /// camera entity, action ids, selection provider (3D F-focus).
        virtual void attach(lux::ecs::Registry& reg) = 0;

        /// Per-frame: input → the camera entity的 transform 组件。跑在
        /// `kPhasePreTransform`——必须先于变换/相机把矩阵算出来。
        ///
        /// ★ 视口宽高不再由参数传进来。此前它是 `SceneTickContext::content_w/h`,
        ///   由宿主每帧转手;而那份数据**世界里本来就有** ——
        ///   `ViewPresentComponent::extent`(批 3:谁在出图、出多大,写在世界里)。
        ///   要用的人自己查。
        virtual void tick(lux::ecs::Registry& reg,
                          const lux::input::ActionMapper& mapper, float dt,
                          float content_w, float content_h) = 0;

        /// True while a mode that owns the mouse is active (3D fly). The host
        /// captures the cursor on this.
        [[nodiscard]] virtual bool wantsCursorCapture() const noexcept { return false; }

        /// Camera world position, read after Schedule refreshed the derived
        /// transform. 2D reports zero (no consumer; streaming is 3D-only).
        [[nodiscard]] virtual Eigen::Vector3f eye(lux::ecs::Registry&) const
        { return Eigen::Vector3f::Zero(); }
        [[nodiscard]] virtual std::optional<lux::math::Position3d>
        worldFocus3D(lux::ecs::Registry&) const
        { return std::nullopt; }
    };

    /// @p selection: the editor's shared Selection (F-focus focal provider) —
    /// HOST state, injected here since it left the shared tick context.
    /// @p camera: 编辑器的脚手架相机实体。与 @p selection 同一条理由 —— 它是**宿主**
    ///            的知识,批 3 把 `camera_entity` 从共享 tick 上下文里删掉了
    ///            (运行时不再镜像「谁在出图」),所以由这里注入。
    [[nodiscard]] std::unique_ptr<IEditorCameraController>
    makeEditorCamera3DNavigator(Selection* selection, lux::ecs::Entity camera);
    [[nodiscard]] std::unique_ptr<IEditorCameraController>
    makeEditorCamera2DNavigator(lux::ecs::Entity camera);

    class CameraSceneSystem final : public lux::ecs::ISystem
    {
    public:
        CameraSceneSystem(std::unique_ptr<IEditorCameraController> navigator,
                          lux::ecs::Entity                     camera,
                          lux::ecs::SystemType                     camera_system);
        ~CameraSceneSystem() override;

        /// 跑在 `kPhasePreTransform`。
        void update(const lux::ecs::SystemUpdateContext& ctx) override;

        /// The editor navigator writes the camera transform before the ordinary
        /// systems derive matrices. Projection aspect itself is owned by those
        /// systems and read from ViewPresentComponent::extent.
        [[nodiscard]] std::span<const lux::ecs::SystemType> runsBefore() const noexcept override
        {
            if (!camera_system_.isValid())
                return {};
            return {&camera_system_, 1u};
        }

        /// 宿主每帧把当前输入 + 视口内容尺寸交给它。两样都是**宿主**的东西
        /// (编辑器的 ActionMapper、ImGui 面板的实际内容区),不在世界里,只能这样递。
        ///
        /// Viewport size remains useful to the 2D camera navigator's zoom
        /// scaling. Projection aspect is not written here.
        void setViewport(const lux::input::ActionMapper* mapper, float w, float h) noexcept
        { mapper_ = mapper; content_w_ = w; content_h_ = h; }

        /// True only while a cursor-owning mode is active (3D fly). Safe before
        /// the first tick (returns false).
        [[nodiscard]] bool wantsCursorCapture() const noexcept;

        /// Camera world position after Schedule::tick。宿主 F-focus 一类的功能读它。
        [[nodiscard]] Eigen::Vector3f eye(lux::ecs::Registry& reg) const;
        [[nodiscard]] std::optional<lux::math::Position3d>
            worldFocus3D(lux::ecs::Registry& reg) const;

    private:
        std::unique_ptr<IEditorCameraController> nav_;
        const lux::input::ActionMapper*          mapper_{nullptr};
        float                                    content_w_{0.f};
        float                                    content_h_{0.f};
        lux::ecs::Entity                     camera_{lux::ecs::kNullEntity};
        lux::ecs::SystemType                     camera_system_{};
        bool                                     attached_{false};
    };

} // namespace lux::editor
