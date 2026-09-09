#pragma once
// Camera math is deliberately independent of UI input event vocabulary.
#include <lux/engine/editor/sessions/scene/SceneSession.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <Eigen/Core>
#include <memory>

namespace lux::editor::sessions
{
    struct CameraMotion final
    {
        Eigen::Vector3d local_translation{Eigen::Vector3d::Zero()};
        Eigen::Vector2d angular_delta{Eigen::Vector2d::Zero()};
        Eigen::Vector2d pan_delta{Eigen::Vector2d::Zero()};
        double dolly{};
    };
    class LUX_EDITOR_SCENE_SESSION_PUBLIC SceneCamera final
    {
    public:
        SceneCamera() noexcept;
        void reset() noexcept;
        [[nodiscard]] SceneResult<void> focus(const Eigen::Vector3d &center, double radius) noexcept;
        [[nodiscard]] SceneResult<void> move(const CameraMotion &) noexcept;
        [[nodiscard]] Eigen::Matrix4d view(const Eigen::Vector3d &origin) const noexcept;
        [[nodiscard]] SceneResult<Eigen::Matrix4d> projection(double aspect) const noexcept;
        [[nodiscard]] Eigen::Vector3d position() const noexcept
        {
            return position_;
        }
        [[nodiscard]] Eigen::Vector3d forward() const noexcept;

    private:
        Eigen::Vector3d position_{Eigen::Vector3d::Zero()};
        double yaw_{}, pitch_{}, vertical_fov_{}, near_plane_{}, far_plane_{};
    };
    struct ViewImageNotice final
    {
        SessionId session;
        rendering::ViewStatus status;
    };

    class LUX_EDITOR_SCENE_SESSION_PUBLIC LUX_OBJECT() SceneView final : public lux::object::Object<SceneView>
    {
    public:
        static const signal_type<ViewImageNotice> imageChanged;
        static SceneResult<std::unique_ptr<SceneView>> create(lux::object::ObjectDispatcherRef, SceneSession &,
                                                              rendering::EditorRenderer &) noexcept;
        ~SceneView() noexcept override;
        SceneView(const SceneView &) = delete;
        SceneView &operator=(const SceneView &) = delete;
        SceneView(SceneView &&) = delete;
        SceneView &operator=(SceneView &&) = delete;
        [[nodiscard]] SessionId sessionId() const noexcept;
        [[nodiscard]] SceneResult<void> resetCamera() noexcept;
        [[nodiscard]] SceneResult<void> synchronize() noexcept; // Owner update only, no Simulation step.
        [[nodiscard]] SceneResult<void> moveCamera(const CameraMotion &) noexcept;
        [[nodiscard]] SceneResult<void> frameSelection() noexcept;
        [[nodiscard]] SceneResult<void> requestExtent(rendering::PixelExtent) noexcept;
        [[nodiscard]] SceneResult<rendering::ViewImage> image() const noexcept;
        [[nodiscard]] SceneResult<void> beginClose() noexcept;
        [[nodiscard]] SceneResult<ECloseProgress> advanceClose() noexcept;

    private:
        struct Impl;
        SceneView(lux::object::ObjectDispatcherRef, std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::sessions
