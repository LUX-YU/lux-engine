#pragma once
#include <lux/engine/math/Ray.hpp>
#include <lux/engine/math/AABB.hpp>
#include <lux/engine/math/Triangle.hpp>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <optional>

namespace lux::math
{
    /**
     * @brief Ray-AABB intersection using the slab method.
     *
     * @param ray   World-space ray (direction must be normalized).
     * @param aabb  Axis-aligned bounding box to test against.
     * @param tMin  [out] Entry distance along the ray (may be negative if origin is inside).
     * @param tMax  [out] Exit distance along the ray.
     * @return true if the ray intersects the AABB (i.e. tMax >= max(tMin, 0)).
     */
    inline bool rayIntersectsAABB(const Ray& ray, const AABB& aabb, float& tMin, float& tMax)
    {
        tMin = -std::numeric_limits<float>::max();
        tMax = std::numeric_limits<float>::max();

        for (int i = 0; i < 3; ++i)
        {
            float invD = 1.0f / ray.direction[i];
            float t0 = (aabb.min[i] - ray.origin[i]) * invD;
            float t1 = (aabb.max[i] - ray.origin[i]) * invD;

            if (invD < 0.0f)
                std::swap(t0, t1);

            tMin = std::max(tMin, t0);
            tMax = std::min(tMax, t1);

            if (tMax < tMin)
                return false;
        }

        // The ray hits if the exit point is in front of the ray origin
        return tMax >= 0.0f;
    }

    /**
     * @brief Möller–Trumbore ray-triangle intersection.
     *
     * @param ray   World-space ray (direction should be normalized).
     * @param tri   Triangle to test against.
     * @return TriangleHit if the ray intersects the front face (t >= 0), nullopt otherwise.
     *
     * Tests front-face only (counter-clockwise winding).
     * Set `cull_backface = false` to test both faces.
     */
    inline std::optional<TriangleHit>
    rayIntersectsTriangle(const Ray& ray, const Triangle& tri, bool cull_backface = false)
    {
        constexpr float kEpsilon = 1e-8f;

        Eigen::Vector3f edge1 = tri.v1 - tri.v0;
        Eigen::Vector3f edge2 = tri.v2 - tri.v0;

        Eigen::Vector3f h = ray.direction.cross(edge2);
        float a = edge1.dot(h);

        if (cull_backface)
        {
            if (a < kEpsilon)
                return std::nullopt; // Back-facing or parallel
        }
        else
        {
            if (std::abs(a) < kEpsilon)
                return std::nullopt; // Parallel
        }

        float f = 1.0f / a;
        Eigen::Vector3f s = ray.origin - tri.v0;
        float u = f * s.dot(h);
        if (u < 0.0f || u > 1.0f)
            return std::nullopt;

        Eigen::Vector3f q = s.cross(edge1);
        float v = f * ray.direction.dot(q);
        if (v < 0.0f || u + v > 1.0f)
            return std::nullopt;

        float t = f * edge2.dot(q);
        if (t < 0.0f)
            return std::nullopt; // Behind the ray

        return TriangleHit{t, u, v};
    }

} // namespace lux::math
