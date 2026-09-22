#include <lux/engine/scene/Camera.hpp>

#include <cmath>
#include <type_traits>

namespace lux::scene
{
lux::cxx::expected<Eigen::Matrix4d, ECameraError> cameraProjection(const Camera &camera, double aspect_ratio) noexcept
{
    if (!std::isfinite(aspect_ratio) || aspect_ratio <= 0.0)
    {
        return lux::cxx::unexpected(ECameraError::INVALID_PROJECTION);
    }

    return std::visit(
        [&](const auto &projection) -> lux::cxx::expected<Eigen::Matrix4d, ECameraError> {
            const double near = projection.near_plane;
            const double far = projection.far_plane;
            if (!std::isfinite(near) || !std::isfinite(far) || near <= 0.0 || far <= near)
            {
                return lux::cxx::unexpected(ECameraError::INVALID_PROJECTION);
            }

            Eigen::Matrix4d result = Eigen::Matrix4d::Zero();
            if constexpr (std::is_same_v<std::decay_t<decltype(projection)>, PerspectiveProjection>)
            {
                const double fov = projection.vertical_fov;
                if (!std::isfinite(fov) || fov <= 0.0 || fov >= std::numbers::pi)
                {
                    return lux::cxx::unexpected(ECameraError::INVALID_PROJECTION);
                }
                const double scale = 1.0 / std::tan(fov * 0.5);
                result(0, 0) = scale / aspect_ratio;
                result(1, 1) = -scale;
                result(2, 2) = far / (near - far);
                result(2, 3) = far * near / (near - far);
                result(3, 2) = -1.0;
            }
            else
            {
                const double extent = projection.vertical_extent;
                if (!std::isfinite(extent) || extent <= 0.0)
                {
                    return lux::cxx::unexpected(ECameraError::INVALID_PROJECTION);
                }
                result(0, 0) = 2.0 / (extent * aspect_ratio);
                result(1, 1) = -2.0 / extent;
                result(2, 2) = 1.0 / (near - far);
                result(2, 3) = near / (near - far);
                result(3, 3) = 1.0;
            }

            if (!result.allFinite())
            {
                return lux::cxx::unexpected(ECameraError::INVALID_PROJECTION);
            }
            return result;
        },
        camera.projection);
}

lux::cxx::expected<Eigen::Matrix4d, ECameraError> cameraView(const Eigen::Affine3d &world,
                                                             const Eigen::Vector3d &origin) noexcept
{
    if (!world.matrix().allFinite() || !origin.allFinite())
    {
        return lux::cxx::unexpected(ECameraError::INVALID_TRANSFORM);
    }

    // Ignore scale, while rejecting degenerate camera bases. This also
    // keeps navigation and picking orthonormal under a scaled parent.
    Eigen::Vector3d forward = -world.linear().col(2);
    Eigen::Vector3d right = forward.cross(world.linear().col(1));
    if (forward.squaredNorm() < 1.0e-24 || right.squaredNorm() < 1.0e-24)
    {
        return lux::cxx::unexpected(ECameraError::INVALID_TRANSFORM);
    }
    forward.normalize();
    right.normalize();
    const Eigen::Vector3d up = right.cross(forward);
    Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
    result.block<1, 3>(0, 0) = right.transpose();
    result.block<1, 3>(1, 0) = up.transpose();
    result.block<1, 3>(2, 0) = -forward.transpose();
    result.block<3, 1>(0, 3) = -result.block<3, 3>(0, 0) * (world.translation() - origin);
    if (!result.allFinite())
    {
        return lux::cxx::unexpected(ECameraError::INVALID_TRANSFORM);
    }
    return result;
}
} // namespace lux::scene
