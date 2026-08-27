#pragma once
#include <Eigen/Core>

namespace lux::math
{
    /**
     * @brief A 3D ray defined by an origin point and a (normalized) direction.
     */
    struct Ray
    {
        Eigen::Vector3f origin = Eigen::Vector3f::Zero();
        Eigen::Vector3f direction = Eigen::Vector3f::UnitZ(); ///< Must be normalized

        /// Evaluate a point along the ray: origin + t * direction.
        [[nodiscard]] Eigen::Vector3f pointAt(float t) const
        {
            return origin + t * direction;
        }
    };

} // namespace lux::math
