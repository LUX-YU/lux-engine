#pragma once
#include <lux/engine/math/Aabb2.hpp>
#include <Eigen/Core>

namespace lux::math
{
    /**
     * @brief 2D circle (the Vector2f sibling of Sphere). A collision primitive
     * and broad-phase bound; bounds() gives its Aabb2 for the physics grid.
     */
    struct Circle2
    {
        Eigen::Vector2f center{Eigen::Vector2f::Zero()};
        float radius{0.0f};

        Circle2() = default;
        Circle2(const Eigen::Vector2f& c, float r) : center(c), radius(r)
        {
        }

        [[nodiscard]] bool isValid() const noexcept
        {
            return radius >= 0.0f;
        }

        [[nodiscard]] bool contains(const Eigen::Vector2f& p) const noexcept
        {
            return (p - center).squaredNorm() <= radius * radius;
        }

        [[nodiscard]] bool intersects(const Circle2& other) const noexcept
        {
            const float rs = radius + other.radius;
            return (center - other.center).squaredNorm() <= rs * rs;
        }

        [[nodiscard]] Aabb2 bounds() const noexcept
        {
            return Aabb2::fromCenterHalf(center, Eigen::Vector2f{radius, radius});
        }
    };

} // namespace lux::math
