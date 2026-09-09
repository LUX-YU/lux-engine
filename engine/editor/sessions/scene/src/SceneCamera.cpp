#include <lux/engine/editor/sessions/scene/SceneView.hpp>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <numbers>
namespace lux::editor::sessions
{
    namespace
    {
        auto invalid() noexcept
        {
            return lux::cxx::unexpected(SceneFailure{ESceneError::INVALID_ARGUMENT});
        }
    } // namespace
    SceneCamera::SceneCamera() noexcept
    {
        reset();
    }
    void SceneCamera::reset() noexcept
    {
        position_ = {6, 4, 8};
        const Eigen::Vector3d direction = (Eigen::Vector3d{0, 0.5, 0} - position_).normalized();
        yaw_ = std::atan2(direction.x(), -direction.z());
        pitch_ = std::asin(direction.y());
        vertical_fov_ = std::numbers::pi / 3;
        near_plane_ = 0.05;
        far_plane_ = 100000;
    }
    Eigen::Vector3d SceneCamera::forward() const noexcept
    {
        return {std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), -std::cos(yaw_) * std::cos(pitch_)};
    }
    SceneResult<void> SceneCamera::focus(const Eigen::Vector3d &center, double radius) noexcept
    {
        if (!center.allFinite() || !std::isfinite(radius) || radius <= 0)
            return invalid();
        const Eigen::Vector3d position = center - forward() * std::max(1.0, radius * 2.5);
        if (!position.allFinite())
            return invalid();
        position_ = position;
        return {};
    }
    SceneResult<void> SceneCamera::move(const CameraMotion &motion) noexcept
    {
        const bool finite = motion.local_translation.allFinite() && motion.angular_delta.allFinite() &&
                            motion.pan_delta.allFinite() && std::isfinite(motion.dolly);
        if (!finite)
            return invalid();
        const auto yaw = std::remainder(yaw_ + motion.angular_delta.x(), 2 * std::numbers::pi);
        const auto pitch = std::clamp(pitch_ + motion.angular_delta.y(), -1.55, 1.55);
        const Eigen::Vector3d forward{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                      -std::cos(yaw) * std::cos(pitch)};
        const Eigen::Vector3d right = forward.cross(Eigen::Vector3d::UnitY()).normalized();
        const Eigen::Vector3d up = right.cross(forward);
        const Eigen::Vector3d next = position_ + right * (motion.local_translation.x() + motion.pan_delta.x()) +
                                     Eigen::Vector3d::UnitY() * motion.local_translation.y() +
                                     forward * (motion.local_translation.z() + motion.dolly) +
                                     up * motion.pan_delta.y();
        if (!next.allFinite() || !std::isfinite(yaw) || !std::isfinite(pitch))
            return invalid();
        position_ = next;
        yaw_ = yaw;
        pitch_ = pitch;
        return {};
    }
    Eigen::Matrix4d SceneCamera::view(const Eigen::Vector3d &origin) const noexcept
    {
        const auto f = forward();
        const Eigen::Vector3d r = f.cross(Eigen::Vector3d::UnitY()).normalized();
        const Eigen::Vector3d u = r.cross(f);
        Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
        result.block<1, 3>(0, 0) = r.transpose();
        result.block<1, 3>(1, 0) = u.transpose();
        result.block<1, 3>(2, 0) = -f.transpose();
        result.block<3, 1>(0, 3) = -result.block<3, 3>(0, 0) * (position_ - origin);
        return result;
    }
    SceneResult<Eigen::Matrix4d> SceneCamera::projection(double aspect) const noexcept
    {
        if (!std::isfinite(aspect) || aspect <= 0)
            return invalid();
        const auto scale = 1 / std::tan(vertical_fov_ / 2);
        Eigen::Matrix4d result = Eigen::Matrix4d::Zero();
        result(0, 0) = scale / aspect;
        result(1, 1) = -scale;
        result(2, 2) = far_plane_ / (near_plane_ - far_plane_);
        result(2, 3) = far_plane_ * near_plane_ / (near_plane_ - far_plane_);
        result(3, 2) = -1;
        if (!result.allFinite())
            return invalid();
        return result;
    }
} // namespace lux::editor::sessions
