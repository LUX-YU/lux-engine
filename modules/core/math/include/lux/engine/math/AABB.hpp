#pragma once
#include <Eigen/Core>
#include <algorithm>
#include <limits>

namespace lux::math
{
    /**
     * @brief Axis-Aligned Bounding Box using Eigen vectors.
     *
     * A general-purpose AABB for geometry queries (ray casting, frustum culling,
     * spatial partitioning).  Keeps min/max as Eigen::Vector3f so it interops
     * naturally with the engine's math stack.
     */
    struct AABB
    {
        Eigen::Vector3f min{
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
        Eigen::Vector3f max{
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max()};

        // ----- Constructors -----

        AABB() = default;
        AABB(const Eigen::Vector3f& mn, const Eigen::Vector3f& mx) : min(mn), max(mx)
        {
        }

        // ----- Queries -----

        [[nodiscard]] bool isValid() const
        {
            return min.x() <= max.x() && min.y() <= max.y() && min.z() <= max.z();
        }

        [[nodiscard]] Eigen::Vector3f center() const
        {
            return (min + max) * 0.5f;
        }
        [[nodiscard]] Eigen::Vector3f extents() const
        {
            return max - min;
        }
        [[nodiscard]] Eigen::Vector3f halfExtents() const
        {
            return (max - min) * 0.5f;
        }

        /// Check whether a point lies inside (or on the boundary of) this AABB.
        [[nodiscard]] bool contains(const Eigen::Vector3f& p) const
        {
            return (p.array() >= min.array()).all() && (p.array() <= max.array()).all();
        }

        /// Check whether this AABB overlaps another AABB.
        [[nodiscard]] bool intersects(const AABB& other) const
        {
            return (max.array() >= other.min.array()).all() && (min.array() <= other.max.array()).all();
        }

        /// Check whether this AABB intersects a sphere.
        [[nodiscard]] bool intersectsSphere(const Eigen::Vector3f& c, float radius) const
        {
            Eigen::Vector3f closest = c.cwiseMax(min).cwiseMin(max);
            return (c - closest).squaredNorm() <= radius * radius;
        }

        // ----- Builders -----

        /// Expand this AABB to include a single point.
        void merge(const Eigen::Vector3f& p)
        {
            min = min.cwiseMin(p);
            max = max.cwiseMax(p);
        }

        /// Expand this AABB to include another AABB.
        void merge(const AABB& other)
        {
            min = min.cwiseMin(other.min);
            max = max.cwiseMax(other.max);
        }

        /// Build from an array of 3D points.
        template <typename Iter> static AABB fromPoints(Iter begin, Iter end)
        {
            AABB box;
            for (auto it = begin; it != end; ++it)
            {
                const Eigen::Vector3f& p = *it;
                box.merge(p);
            }
            return box;
        }

        // ----- Transform -----

        /**
         * @brief Transform a local-space AABB into world-space using an affine Matrix4f.
         *
         * Uses the Arvo 1990 min/max projection technique — no 8-corner expansion,
         * just 3×3 element-wise min/max accumulation from the rotation/scale sub-matrix
         * plus the translation column.
         */
        [[nodiscard]] AABB transformed(const Eigen::Matrix4f& world) const
        {
            AABB out;
            // Translation column
            Eigen::Vector3f t = world.block<3, 1>(0, 3);
            out.min = t;
            out.max = t;

            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                {
                    float a = world(i, j) * min[j];
                    float b = world(i, j) * max[j];
                    out.min[i] += std::min(a, b);
                    out.max[i] += std::max(a, b);
                }
            }
            return out;
        }
    };

} // namespace lux::math
