#pragma once
#include <lux/engine/math/AABB.hpp>
#include <lux/engine/math/Ray.hpp>
#include <lux/engine/math/Triangle.hpp>
#include <lux/engine/math/Intersection.hpp>

#include <Eigen/Core>
#include <vector>
#include <algorithm>
#include <optional>
#include <cstdint>
#include <cassert>
#include <limits>

namespace lux::math
{
    /**
     * @brief Result of a BVH ray-cast query.
     */
    struct BVHHit
    {
        float t;                 ///< Distance along the ray
        float u, v;              ///< Barycentric coordinates on the triangle
        uint32_t triangle_index; ///< Index of the hit triangle (in the original index buffer)
    };

    /**
     * @brief Per-mesh Bounding Volume Hierarchy for precise ray-triangle picking.
     *
     * Built from a mesh's vertex positions + index buffer at load time.
     * Uses a flat node array (cache-friendly) with a binary tree layout.
     * Leaf nodes hold a contiguous range of triangle indices.
     *
     * Intended as a second-stage test after the coarse per-part AABB test:
     *
     *   1) Per-part AABB test (existing, fast O(N) over parts)
     *   2) Per-mesh BVH traversal (this class, O(log T) over triangles)
     *
     * The BVH operates in **local (model) space**.
     */
    class MeshBVH
    {
    public:
        MeshBVH() = default;

        /**
         * @brief Build the BVH from raw mesh data.
         *
         * @param positions    Pointer to Vector3f positions (or float[3]-compatible).
         * @param vertex_count Number of vertices.
         * @param indices      Triangle index buffer (3 indices per triangle).
         * @param index_count  Number of indices (must be divisible by 3).
         * @param max_leaf_tris Maximum triangles per leaf node (default 4).
         */
        void build(
            const Eigen::Vector3f* positions,
            uint32_t vertex_count,
            const uint32_t* indices,
            uint32_t index_count,
            uint32_t max_leaf_tris = 4
        )
        {
            (void)vertex_count;
            assert(index_count % 3 == 0);
            const uint32_t tri_count = index_count / 3;
            if (tri_count == 0)
                return;

            max_leaf_tris_ = std::max(max_leaf_tris, 1u);

            // Gather triangle data into parallel arrays
            tris_.resize(tri_count);
            centroids_.resize(tri_count);
            orig_tri_idx_.resize(tri_count);

            for (uint32_t i = 0; i < tri_count; ++i)
            {
                uint32_t i0 = indices[i * 3 + 0];
                uint32_t i1 = indices[i * 3 + 1];
                uint32_t i2 = indices[i * 3 + 2];
                tris_[i] = {positions[i0], positions[i1], positions[i2]};
                centroids_[i] = (positions[i0] + positions[i1] + positions[i2]) / 3.0f;
                orig_tri_idx_[i] = i;
            }

            // Reserve nodes (at most ~2N - 1)
            nodes_.clear();
            nodes_.reserve(tri_count * 2);

            buildRecursive(0, tri_count);
        }

        /**
         * @brief Test a **local-space** ray against the BVH.
         * @return The closest BVHHit, or nullopt.
         */
        [[nodiscard]] std::optional<BVHHit> intersectLocal(const Ray& local_ray) const
        {
            if (nodes_.empty())
                return std::nullopt;

            BVHHit best{};
            best.t = std::numeric_limits<float>::max();
            bool found = false;

            traverse(0, local_ray, best, found);

            return found ? std::optional<BVHHit>(best) : std::nullopt;
        }

        /**
         * @brief Test a world-space ray by transforming it into local space.
         *
         * @param world_ray  Ray in world space.
         * @param world      The entity's model-to-world matrix.
         * @return The closest BVHHit (t in world-space units), or nullopt.
         */
        [[nodiscard]] std::optional<BVHHit> intersect(const Ray& world_ray, const Eigen::Matrix4f& world) const
        {
            Eigen::Matrix4f inv = world.inverse();

            // Transform ray into local space
            Eigen::Vector4f o4 =
                inv * Eigen::Vector4f(world_ray.origin.x(), world_ray.origin.y(), world_ray.origin.z(), 1.0f);
            Eigen::Vector4f d4 =
                inv * Eigen::Vector4f(world_ray.direction.x(), world_ray.direction.y(), world_ray.direction.z(), 0.0f);

            Ray local_ray;
            local_ray.origin = o4.head<3>();
            Eigen::Vector3f ld = d4.head<3>();
            float scale = ld.norm();
            if (scale < 1e-12f)
                return std::nullopt;
            local_ray.direction = ld / scale;

            auto hit = intersectLocal(local_ray);
            if (hit)
            {
                // local_t * |local_dir_unnorm| = world distance
                // (input world_ray.direction is unit-length)
                hit->t *= scale;
            }
            return hit;
        }

        [[nodiscard]] uint32_t triangleCount() const
        {
            return static_cast<uint32_t>(tris_.size());
        }
        [[nodiscard]] uint32_t nodeCount() const
        {
            return static_cast<uint32_t>(nodes_.size());
        }
        [[nodiscard]] bool isBuilt() const
        {
            return !nodes_.empty();
        }

    private:
        // ------- Flat node -------
        struct Node
        {
            AABB bounds;
            uint32_t first; ///< Interior: left child index.  Leaf: first tri index.
            uint32_t count; ///< 0 = interior node.  >0 = leaf (triangle count).
            uint32_t right; ///< Interior: right child index.  Leaf: unused.
        };

        // ------- Build -------
        uint32_t buildRecursive(uint32_t begin, uint32_t end)
        {
            uint32_t idx = static_cast<uint32_t>(nodes_.size());
            nodes_.push_back({});

            // Compute bounds over [begin, end)
            AABB box;
            for (uint32_t i = begin; i < end; ++i)
            {
                box.merge(tris_[i].v0);
                box.merge(tris_[i].v1);
                box.merge(tris_[i].v2);
            }
            nodes_[idx].bounds = box;

            uint32_t n = end - begin;
            if (n <= max_leaf_tris_)
            {
                // Leaf
                nodes_[idx].first = begin;
                nodes_[idx].count = n;
                nodes_[idx].right = 0;
                return idx;
            }

            // Pick split axis = longest extent
            Eigen::Vector3f ext = box.extents();
            int axis = 0;
            if (ext.y() > ext.x())
                axis = 1;
            if (ext.z() > ext[axis])
                axis = 2;

            // Centroid mean along axis
            float mid = 0.0f;
            for (uint32_t i = begin; i < end; ++i)
                mid += centroids_[i][axis];
            mid /= static_cast<float>(n);

            // Partition parallel arrays (tris_, centroids_, orig_tri_idx_)
            uint32_t split = begin;
            for (uint32_t i = begin; i < end; ++i)
            {
                if (centroids_[i][axis] < mid)
                {
                    if (i != split)
                    {
                        std::swap(tris_[i], tris_[split]);
                        std::swap(centroids_[i], centroids_[split]);
                        std::swap(orig_tri_idx_[i], orig_tri_idx_[split]);
                    }
                    ++split;
                }
            }

            // Degenerate: everything fell on one side -> split in half
            if (split == begin || split == end)
                split = begin + n / 2;

            // Build children (nodes_ may reallocate, so use idx not reference)
            uint32_t left = buildRecursive(begin, split);
            uint32_t right = buildRecursive(split, end);

            nodes_[idx].first = left;
            nodes_[idx].right = right;
            nodes_[idx].count = 0; // interior
            return idx;
        }

        // ------- Traversal -------
        void traverse(uint32_t idx, const Ray& ray, BVHHit& best, bool& found) const
        {
            const Node& nd = nodes_[idx];

            float tMin, tMax;
            if (!rayIntersectsAABB(ray, nd.bounds, tMin, tMax))
                return;
            if (tMin > best.t) // entire node is farther than current best
                return;

            if (nd.count > 0)
            {
                // Leaf — test triangles
                for (uint32_t i = 0; i < nd.count; ++i)
                {
                    uint32_t ti = nd.first + i;
                    auto hit = rayIntersectsTriangle(ray, tris_[ti]);
                    if (hit && hit->t < best.t)
                    {
                        best.t = hit->t;
                        best.u = hit->u;
                        best.v = hit->v;
                        best.triangle_index = orig_tri_idx_[ti];
                        found = true;
                    }
                }
            }
            else
            {
                // Interior — traverse both children
                traverse(nd.first, ray, best, found);
                traverse(nd.right, ray, best, found);
            }
        }

        // ------- Data -------
        std::vector<Node> nodes_;
        std::vector<Triangle> tris_;             ///< Permuted triangle data
        std::vector<Eigen::Vector3f> centroids_; ///< Parallel to tris_
        std::vector<uint32_t> orig_tri_idx_;     ///< Original triangle index
        uint32_t max_leaf_tris_{4};
    };

} // namespace lux::math
