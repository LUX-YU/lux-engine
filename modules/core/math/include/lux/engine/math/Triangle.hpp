#pragma once
#include <Eigen/Core>

namespace lux::math
{
    /**
     * @brief A triangle defined by three vertices, with optional index metadata.
     */
    struct Triangle
    {
        Eigen::Vector3f v0, v1, v2;
    };

    /**
     * @brief Result of a ray-triangle intersection test.
     */
    struct TriangleHit
    {
        float t;       ///< Distance along the ray to the intersection point
        float u, v;    ///< Barycentric coordinates (w = 1 - u - v)
    };

} // namespace lux::math
