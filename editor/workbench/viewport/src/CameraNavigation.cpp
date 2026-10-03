#include <lux/engine/editor/views/CameraNavigation.hpp>
#include <lux/engine/math/Picking.hpp>
#include <Eigen/LU>
#include <algorithm>
#include <cmath>
#include <numbers>
namespace lux::editor::views
{
    namespace
    {
        auto invalid(std::string_view reason)
        {
            return lux::cxx::unexpected(reason);
        }
    }
    CameraNavigationResult<ViewportCameraState> navigateCamera(
        const lux::simulation::ecs::Transform3D& source,
        const lux::scene::Camera& camera,
        const CameraMotion& motion
    )
    {
        if (!motion.local_translation.allFinite() || !motion.angular_delta.allFinite() ||
            !motion.pan_delta.allFinite() || !std::isfinite(motion.dolly))
        {
            return invalid("Camera or motion is invalid");
        }
        auto pose = source;
        auto projection = camera;
        const Eigen::Vector3d old_forward = pose.rotation * -Eigen::Vector3d::UnitZ();
        const double yaw = std::remainder(
            std::atan2(old_forward.x(), -old_forward.z()) + motion.angular_delta.x(),
            2.0 * std::numbers::pi
        );
        const double pitch =
            std::clamp(std::asin(std::clamp(old_forward.y(), -1.0, 1.0)) + motion.angular_delta.y(), -1.55, 1.55);
        const Eigen::Vector3d forward{
            std::sin(yaw) * std::cos(pitch),
            std::sin(pitch),
            -std::cos(yaw) * std::cos(pitch)
        };
        const Eigen::Vector3d right = forward.cross(Eigen::Vector3d::UnitY()).normalized();
        const Eigen::Vector3d up = right.cross(forward);
        Eigen::Matrix3d basis;
        basis.col(0) = right;
        basis.col(1) = up;
        basis.col(2) = -forward;
        pose.rotation = Eigen::Quaterniond(basis);
        double dolly = motion.dolly;
        if (auto* orthographic = std::get_if<lux::scene::OrthographicProjection>(&projection.projection))
        {
            orthographic->vertical_extent =
                std::clamp(orthographic->vertical_extent * std::exp(-0.1 * dolly), 0.01, 1.0e9);
            dolly = 0;
        }
        pose.translation += right * (motion.local_translation.x() + motion.pan_delta.x()) +
                            Eigen::Vector3d::UnitY() * motion.local_translation.y() +
                            forward * (motion.local_translation.z() + dolly) + up * motion.pan_delta.y();
        return ViewportCameraState{pose, projection};
    }

    CameraNavigationResult<lux::math::Ray3d> cameraRay(
        const lux::simulation::ecs::WorldTransform3D& pose,
        const lux::scene::Camera& camera,
        Eigen::Vector2d point,
        Eigen::Vector2d extent
    )
    {
        if (!point.allFinite() || !extent.allFinite() || (extent.array() <= 0).any() || (point.array() < 0).any() ||
            (point.array() > extent.array()).any())
        {
            return invalid("The image or camera is not ready for a ray query");
        }
        const Eigen::Vector3d origin = pose.value.translation();
        const auto view = lux::scene::cameraView(pose.value, origin);
        const auto projection = lux::scene::cameraProjection(camera, extent.x() / extent.y());
        if (!view || !projection)
        {
            return invalid("Camera projection or transform is invalid");
        }
        const Eigen::Matrix4d inverse = (*projection * *view).inverse();
        lux::math::Ray3d result;
        lux::math::screenToRay(point.x(), point.y(), extent.x(), extent.y(), inverse, result);
        result.origin += origin;
        if (!result.origin.allFinite() || !result.direction.allFinite())
        {
            return invalid("The projection did not produce a finite ray");
        }
        return result;
    }

}
