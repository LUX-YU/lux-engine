#pragma once
#include <Eigen/Core>
#include <algorithm>
#include <limits>

namespace lux::math
{
    /**
     * @brief 2D axis-aligned bounding box (the Vector2f sibling of AABB).
     *
     * Shared vocabulary between the 2D domain (images/tilemaps/pixel fields)
     * and physics: a collision shape's broad-phase bound is an Aabb2, exactly
     * as a 3D mesh's cull bound is an AABB. Keeps min/max as Eigen::Vector2f so
     * it interops with the engine's math stack. Same method surface as AABB.
     */
    struct Aabb2
    {
        Eigen::Vector2f min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        Eigen::Vector2f max{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};

        // ----- Constructors -----

        Aabb2() = default;
        Aabb2(const Eigen::Vector2f& mn, const Eigen::Vector2f& mx) : min(mn), max(mx)
        {
        }

        /// Build from a centre + half-extents (the collider-authoring form).
        [[nodiscard]] static Aabb2 fromCenterHalf(const Eigen::Vector2f& c, const Eigen::Vector2f& half)
        {
            return Aabb2{c - half, c + half};
        }

        // ----- Queries -----

        [[nodiscard]] bool isValid() const
        {
            return min.x() <= max.x() && min.y() <= max.y();
        }

        [[nodiscard]] Eigen::Vector2f center() const
        {
            return (min + max) * 0.5f;
        }
        [[nodiscard]] Eigen::Vector2f extents() const
        {
            return max - min;
        }
        [[nodiscard]] Eigen::Vector2f halfExtents() const
        {
            return (max - min) * 0.5f;
        }

        /// Point inside (or on the boundary).
        [[nodiscard]] bool contains(const Eigen::Vector2f& p) const
        {
            return (p.array() >= min.array()).all() && (p.array() <= max.array()).all();
        }

        /// Overlaps another AABB (touching counts — closed intervals).
        [[nodiscard]] bool intersects(const Aabb2& other) const
        {
            return (max.array() >= other.min.array()).all() && (min.array() <= other.max.array()).all();
        }

        /// Strict overlap (open intervals) — the falling-sand / collision form
        /// where a shared edge does NOT count as overlapping. Matches the
        /// previous pack-side Aabb2::overlaps semantics exactly.
        [[nodiscard]] bool overlaps(const Aabb2& o) const
        {
            return min.x() < o.max.x() && max.x() > o.min.x() && min.y() < o.max.y() && max.y() > o.min.y();
        }

        /// Intersects a circle.
        [[nodiscard]] bool intersectsCircle(const Eigen::Vector2f& c, float radius) const
        {
            Eigen::Vector2f closest = c.cwiseMax(min).cwiseMin(max);
            return (c - closest).squaredNorm() <= radius * radius;
        }

        // ----- Builders -----

        void merge(const Eigen::Vector2f& p)
        {
            min = min.cwiseMin(p);
            max = max.cwiseMax(p);
        }
        void merge(const Aabb2& other)
        {
            min = min.cwiseMin(other.min);
            max = max.cwiseMax(other.max);
        }

        template <typename Iter> static Aabb2 fromPoints(Iter begin, Iter end)
        {
            Aabb2 box;
            for (auto it = begin; it != end; ++it)
                box.merge(*it);
            return box;
        }
    };

} // namespace lux::math
