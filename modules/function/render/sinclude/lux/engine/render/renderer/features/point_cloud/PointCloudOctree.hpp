#pragma once
#include <lux/engine/function/visibility.h>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>
#include <cmath>
#include <algorithm>
#include <memory>

#include <Eigen/Core>
#include <lux/engine/math/AABB.hpp>
#include <lux/engine/render/renderer/FrustumCuller.hpp>

namespace lux::render::pc
{
    struct Point
    {
        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        float   r = 1.0f;
        float   g = 1.0f;
        float   b = 1.0f;
        float   intensity = 1.0f;
        uint64_t id = 0;
    };

    /// pc::AABB is now an alias for the unified math::AABB.
    using AABB = lux::math::AABB;

    class LUX_FUNCTION_PUBLIC PointCloudOctree
    {
    public:
        using NodeId = uint32_t;
        static constexpr NodeId kInvalidNode = std::numeric_limits<NodeId>::max();

        struct Config
        {
            AABB     root_bounds;           // Root node spatial bounds
            uint32_t max_depth = 10;        // Maximum depth
            uint32_t max_points_per_leaf = 512; // Max points per leaf; exceeds triggers split
        };

    private:
        struct Node
        {
            AABB bounds;
            std::array<NodeId, 8> children{}; // Child node indices; kInvalidNode means empty
            bool is_leaf = true;
            uint32_t depth = 0;               // Node depth in tree (CQ-5 per-level lock)

            std::vector<Point>   points;      // Only used on leaf nodes
            std::vector<uint8_t> alive;       // Whether each point is valid (1=alive, 0=deleted)
            uint32_t alive_count_ = 0;        // O(1) alive count — updated incrementally

            bool dirty_cpu_to_gpu = false;    // CPU -> GPU sync dirty flag
            bool need_compact = false;   // Whether compaction is needed (remove dead points)
            bool active = true;          // false = orphaned after pruneEmptyLeaves() — skip everywhere

            Node()
            {
                children.fill(kInvalidNode);
            }

            size_t aliveCount() const
            {
                return alive_count_;
            }

            bool empty() const
            {
                return alive_count_ == 0;
            }
        };

    public:
        /// Read-only deep-copy of a single leaf node.
        /// Owns its data — safe to use after octree locks are released.
        ///
        /// T2-5: When data_valid == false this is a metadata-only stub.
        ///   points and alive are empty; use cached_alive_count for the live-point
        ///   count.  The stub is still included in the Snapshot so that
        ///   syncFromSnapshot knows the GPU chunk for this node is still active.
        struct LeafView
        {
            NodeId               id = kInvalidNode;
            std::vector<Point>   points;           // deep-copied point data (empty when !data_valid)
            std::vector<uint8_t> alive;            // deep-copied alive flags (empty when !data_valid)
            AABB                 bounds;
            bool                 dirty_cpu_to_gpu = false;
            bool                 data_valid       = true;   ///< false → metadata-only stub, no deep copy
            uint32_t             cached_alive_count = 0;   ///< pre-computed alive count (valid always)

            [[nodiscard]] size_t count()  const noexcept { return points.size(); }
            [[nodiscard]] bool   empty()  const noexcept { return points.empty(); }
        };

        /// Snapshot of all visible leaves.
        /// Deep-copies leaf data into owned vectors.
        /// Must be called only from the render thread.
        struct Snapshot
        {
            std::vector<LeafView> leaves;
            uint64_t              version = 0;
        };

    public:
        explicit PointCloudOctree(const Config& cfg);

        void clear();

        const Config& config() const { return config_; }

        uint64_t insertPoint(Point p);
        void insertPoints(const std::vector<Point>& pts);

        size_t markPointsInAABBDeleted(const AABB& region);
        size_t markPointsInSphereDeleted(const Eigen::Vector3f& center, float radius);
        size_t compactDirtyNodes();

        /// Collapse internal nodes whose every active-leaf child is empty.
        /// Orphaned child nodes are marked inactive and ignored by all traversals.
        /// Iterates until no further collapses are possible (handles multi-level gaps).
        /// Call after compactDirtyNodes() to keep the tree tidy.
        /// @return Number of internal nodes collapsed.
        size_t pruneEmptyLeaves();

        void markLeavesSynced(std::span<const NodeId> leaf_ids);
        void markAllLeavesSynced();
        /// Mark every active leaf as dirty (CPU→GPU flag set).
        /// Call this when switching render modes so the new mode's sync client
        /// performs a full resync on its first flush().
        void markAllLeavesDirty();

        // ========== LOD Traversal (Phase 2) ==========

        /// Result of a LOD-aware traversal for rendering.
        struct LODTraversalResult
        {
            std::vector<NodeId> visible_nodes;  ///< Nodes that should be rendered this frame
            std::vector<NodeId> to_split;       ///< Leaf nodes needing child loading (too coarse)
            std::vector<NodeId> to_merge;       ///< Child nodes that can be evicted (parent sufficient)
        };

        /// @brief Perform view-dependent LOD traversal (Potree-style).
        ///
        /// Traverses the octree from root, deciding for each node whether to:
        /// - Render it (add to visible_nodes)
        /// - Split it (request child loading)
        /// - Merge children (no longer needed at this distance)
        ///
        /// @param view_proj 4x4 column-major view-projection matrix (16 floats)
        /// @param camera_pos Camera world position (3 floats)
        /// @param screen_width Viewport width in pixels
        /// @param pixel_error_threshold Maximum allowed pixel error (default 1.0)
        /// @return LODTraversalResult with rendering decisions
        LODTraversalResult traverseForLOD(
            const float* view_proj,
            const float* camera_pos,
            float screen_width,
            float pixel_error_threshold = 1.0f) const;

        /// Generate a read-only snapshot for the render thread.
        ///
        /// @param skip_empty_leaves  If true, leaves with no alive points are omitted entirely.
        /// @param dirty_only         T2-5 optimisation: when true, only dirty leaves receive a
        ///                           full deep copy of their point data.  All other non-empty
        ///                           leaves are included as lightweight metadata-only stubs
        ///                           (data_valid == false) so that the GPU chunk manager can
        ///                           keep their chunks alive without re-uploading anything.
        Snapshot captureSnapshot(bool skip_empty_leaves = true, bool dirty_only = false) const;

        /// @brief Zero-copy iteration over dirty leaf nodes.
        ///
        /// Avoids the deep-copy overhead of captureSnapshot() by passing
        /// a direct read-only reference to each dirty leaf's internal
        /// arrays.  The callback receives stable references that remain
        /// valid for the duration of the call.
        ///
        /// Callback signature:
        /// @code
        ///   void fn(NodeId id, const AABB& bounds,
        ///           std::span<const Point>   points,
        ///           std::span<const uint8_t> alive,
        ///           uint32_t alive_count)
        /// @endcode
        ///
        /// Non-dirty leaves are still reported if @p all_metadata is true,
        /// but with empty spans and alive_count only (metadata stub).
        ///
        /// @param callback        Called once per active leaf.
        /// @param skip_empty      If true, skip leaves with alive_count == 0.
        /// @param all_metadata    If true, also call fn for non-dirty leaves
        ///                        with empty spans (metadata-only).
        template <typename Func>
        void forEachDirtyLeaf(Func&& callback,
                              bool skip_empty = true,
                              bool all_metadata = true) const
        {
            for (NodeId id = 0; id < static_cast<NodeId>(nodes_.size()); ++id)
            {
                const Node& node = nodes_[id];
                if (!node.active || !node.is_leaf) continue;
                const uint32_t ac = node.alive_count_;
                if (skip_empty && ac == 0) continue;

                if (node.dirty_cpu_to_gpu)
                {
                    callback(id, node.bounds,
                             std::span<const Point>(node.points),
                             std::span<const uint8_t>(node.alive),
                             ac);
                }
                else if (all_metadata)
                {
                    callback(id, node.bounds,
                             std::span<const Point>{},
                             std::span<const uint8_t>{},
                             ac);
                }
            }
        }

        // Traverse all points within AABB (read-only), process in callback
        template <typename Func>
        void forEachPointInAABB(const AABB& region, Func&& fn) const
        {
            if (root_ == kInvalidNode) return;
            forEachPointInAABBRecursive(root_, region, fn);
        }

        /**
         * @brief Iterate over every node in the octree (read-only).
         *
         * Callback signature: @code void fn(const AABB& bounds, uint32_t depth, bool is_leaf) @endcode
         *
         * All nodes are visited regardless of whether they contain alive points.
         * Must be called only from the render thread.
         */
        template <typename Func>
        void forAllNodes(Func&& fn) const
        {
            for (const Node& n : nodes_)
            {
                if (!n.active)
                    continue;
                fn(n.bounds, n.depth, n.is_leaf, static_cast<uint32_t>(n.aliveCount()));
            }
        }

        // ========== Diagnostics ==========

        /// Count total alive points across all leaf nodes
        uint64_t totalAlivePoints() const;

        /// Count total leaf nodes (non-empty)
        uint32_t leafCount() const;

        /// Total node count (internal + leaf)
        uint32_t nodeCount() const;

    private:
        Config                         config_{};
        std::vector<Node>              nodes_;
        std::vector<NodeId>            node_free_list_;  ///< recycled orphan slots — used by allocateNode() before emplace_back
        NodeId                         root_ = kInvalidNode;

        // NOTE: No mutex. The octree is accessed exclusively on the render thread.
        // Cross-thread sync goes through PointCloudSyncClient + FramePack;
        // render-thread feature phases only consume immutable snapshots.
        uint64_t                       version_ = 0;      // Incremented on every structure/point-set modification
        uint64_t                       next_point_id_ = 1;

    private:
        // ========== LOD Traversal Helpers ==========

        static float computeNodeScreenError(
            const AABB& bounds,
            const float* view_proj,
            const float* camera_pos,
            float screen_width);

        void traverseLODRecursive(
            NodeId node_id,
            const Frustum& frustum,
            const float* view_proj,
            const float* camera_pos,
            float screen_width,
            float pixel_error_threshold,
            LODTraversalResult& result) const;

        bool hasAnyChild(NodeId node_id) const;

        NodeId allocateNode();

        static AABB computeChildBounds(const AABB& parent, int child_index);
        static int childIndexForPosition(const AABB& bounds, const Eigen::Vector3f& pos);

        void insertPointInternal(NodeId node_id, const Point& p, uint32_t depth);
        void splitLeaf(NodeId node_id, uint32_t depth);

        template <typename Func>
        void forEachPointInAABBRecursive(NodeId node_id, const AABB& region, Func& fn) const
        {
            const Node& node = nodes_[node_id];
            if (!node.bounds.intersects(region))
                return;

            if (node.is_leaf)
            {
                for (size_t i = 0; i < node.points.size(); ++i)
                {
                    if (!node.alive[i])
                        continue;
                    const Point& p = node.points[i];
                    if (region.contains(p.position))
                    {
                        fn(p);
                    }
                }
            }
            else
            {
                for (int i = 0; i < 8; ++i)
                {
                    NodeId child_id = node.children[i];
                    if (child_id == kInvalidNode)
                        continue;
                    forEachPointInAABBRecursive(child_id, region, fn);
                }
            }
        }

        template <typename Predicate>
        size_t markDeletedRecursive(NodeId node_id, Predicate&& pred,
            const Eigen::Vector3f* sphere_center = nullptr,
            float sphere_radius = 0.0f)
        {
            Node& node = nodes_[node_id];

            // Early-out based on AABB test
            if (sphere_center)
            {
                if (!node.bounds.intersectsSphere(*sphere_center, sphere_radius))
                    return 0;
            }

            size_t removed = 0;

            if (node.is_leaf)
            {
                for (size_t i = 0; i < node.points.size(); ++i)
                {
                    if (!node.alive[i])
                        continue;

                    if (pred(node.points[i]))
                    {
                        node.alive[i] = 0;
                        --node.alive_count_;
                        node.need_compact = true;
                        node.dirty_cpu_to_gpu = true;
                        ++removed;
                    }
                }
            }
            else
            {
                for (int i = 0; i < 8; ++i)
                {
                    NodeId child_id = node.children[i];
                    if (child_id == kInvalidNode)
                        continue;
                    removed += markDeletedRecursive(child_id, pred, sphere_center, sphere_radius);
                }
            }

            return removed;
        }
    };

} // namespace lux::render::pc
