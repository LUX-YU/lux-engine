#pragma once

#include <lux/engine/math/AABB.hpp>

#include <Eigen/Core>
#include <algorithm>
#include <cmath>

namespace lux::math
{
    /**
     * @brief General-purpose bounding sphere.
     *
     * Useful for broad-phase intersection tests, frustum culling, and
     * conservative object bounds in render pipelines.
     */
    struct Sphere
    {
        Eigen::Vector3f center{Eigen::Vector3f::Zero()};
        float radius{0.0f};

        Sphere() = default;
        Sphere(const Eigen::Vector3f& c, float r) : center(c), radius(r)
        {
        }

        [[nodiscard]] bool isValid() const noexcept
        {
            return std::isfinite(radius) && radius >= 0.0f;
        }

        [[nodiscard]] bool contains(const Eigen::Vector3f& p) const noexcept
        {
            const float rr = radius * radius;
            return (p - center).squaredNorm() <= rr;
        }

        [[nodiscard]] bool intersects(const Sphere& other) const noexcept
        {
            const float rs = radius + other.radius;
            return (other.center - center).squaredNorm() <= rs * rs;
        }

        [[nodiscard]] bool intersects(const AABB& aabb) const noexcept
        {
            return aabb.intersectsSphere(center, radius);
        }

        /**
         * @brief Transform this sphere by an affine matrix.
         *
         * Radius is scaled by the maximum length among the 3 basis vectors to
         * remain conservative under non-uniform scaling.
         */
        [[nodiscard]] Sphere transformed(const Eigen::Matrix4f& world) const noexcept
        {
            Eigen::Vector4f c4(center.x(), center.y(), center.z(), 1.0f);
            const Eigen::Vector4f wc4 = world * c4;

            const Eigen::Matrix3f m = world.block<3, 3>(0, 0);
            const float sx = m.col(0).norm();
            const float sy = m.col(1).norm();
            const float sz = m.col(2).norm();
            const float s = std::max({sx, sy, sz});

            return Sphere{Eigen::Vector3f(wc4.x(), wc4.y(), wc4.z()), radius * s};
        }
    };

} // namespace lux::math
