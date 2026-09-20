#pragma once
#include <Eigen/Core>

namespace lux::math
{
    /**
     * @brief A 3D ray defined by an origin point and a (normalized) direction.
     */
    template <class Scalar> struct BasicRay
    {
        using Vector = Eigen::Matrix<Scalar, 3, 1>;
        Vector origin = Vector::Zero();
        Vector direction = Vector::UnitZ(); ///< Must be normalized

        /// Evaluate a point along the ray: origin + t * direction.
        [[nodiscard]] Vector pointAt(Scalar t) const
        {
            return origin + t * direction;
        }
    };

    using Ray = BasicRay<float>;
    using Ray3d = BasicRay<double>;

} // namespace lux::math
