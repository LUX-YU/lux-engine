#pragma once

#include <lux/engine/editor/DocumentRequests.hpp>
#include <Eigen/Core>

namespace lux::editor::gui
{
    struct CameraMotion final
    {
        Eigen::Vector3d local_translation{Eigen::Vector3d::Zero()};
        Eigen::Vector2d angular_delta{Eigen::Vector2d::Zero()};
        Eigen::Vector2d pan_delta{Eigen::Vector2d::Zero()};
        double dolly{};
    };

    class SceneCamera final
    {
      public:
        SceneCamera() noexcept;
        void reset() noexcept;
        [[nodiscard]] EditorResult<void> focus(const Eigen::Vector3d &, double radius) noexcept;
        [[nodiscard]] EditorResult<void> move(const CameraMotion &) noexcept;
        [[nodiscard]] Eigen::Matrix4d view(const Eigen::Vector3d &) const noexcept;
        [[nodiscard]] EditorResult<Eigen::Matrix4d> projection(double aspect) const noexcept;

        [[nodiscard]] Eigen::Vector3d position() const noexcept
        {
            return position_;
        }

        [[nodiscard]] Eigen::Vector3d forward() const noexcept;

      private:
        Eigen::Vector3d position_{Eigen::Vector3d::Zero()};
        double yaw_{}, pitch_{}, vertical_fov_{}, near_plane_{}, far_plane_{};
    };
} // namespace lux::editor::gui
