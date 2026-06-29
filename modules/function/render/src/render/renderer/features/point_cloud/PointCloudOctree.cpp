#include <lux/engine/render/renderer/features/point_cloud/PointCloudOctree.hpp>
#include <cassert>

namespace lux::render::pc
{
    PointCloudOctree::PointCloudOctree(const Config& cfg)
        : config_(cfg)
    {
        if (!config_.root_bounds.isValid())
        {
            config_.root_bounds = AABB(Eigen::Vector3f(-1, -1, -1), Eigen::Vector3f(1, 1, 1));
        }

        nodes_.reserve(1024);
        root_ = allocateNode();
        nodes_[root_].bounds = config_.root_bounds;
        nodes_[root_].is_leaf = true;
        nodes_[root_].depth = 0;
    }

    void PointCloudOctree::clear()
    {
        nodes_.clear();
        nodes_.reserve(1024);
        root_ = allocateNode();
        nodes_[root_].bounds = config_.root_bounds;
        nodes_[root_].is_leaf = true;
        nodes_[root_].depth = 0;
        version_++;
        next_point_id_ = 1;
    }

    uint64_t PointCloudOctree::insertPoint(Point p)
    {
        if (p.id == 0)
        {
            p.id = next_point_id_++;
        }

        insertPointInternal(root_, p, 0);
        version_++;
        return p.id;
    }

    void PointCloudOctree::insertPoints(const std::vector<Point>& pts)
    {
        for (Point p : pts)
        {
            if (p.id == 0)
                p.id = next_point_id_++;
            insertPointInternal(root_, p, 0);
        }
        version_++;
    }

    size_t PointCloudOctree::markPointsInAABBDeleted(const AABB& region)
    {
        if (root_ == kInvalidNode) return 0;
        size_t removed = markDeletedRecursive(root_, [&](const Point& p) {
            return region.contains(p.position);
            });
        if (removed > 0)
            version_++;
        return removed;
    }

    size_t PointCloudOctree::markPointsInSphereDeleted(const Eigen::Vector3f& center, float radius)
    {
        if (root_ == kInvalidNode) return 0;
        float r2 = radius * radius;
        size_t removed = markDeletedRecursive(root_, [&](const Point& p) {
            return (p.position - center).squaredNorm() <= r2;
            }, &center, radius);
        if (removed > 0)
            version_++;
        return removed;
    }

    size_t PointCloudOctree::compactDirtyNodes()
    {
        size_t total_removed = 0;
        for (Node& node : nodes_)
        {
            if (!node.is_leaf || !node.need_compact)
                continue;

            std::vector<Point>   new_points;
            std::vector<uint8_t> new_alive;
            new_points.reserve(node.points.size());
            new_alive.reserve(node.alive.size());

            for (size_t i = 0; i < node.points.size(); ++i)
            {
                if (node.alive[i])
                {
                    new_points.push_back(node.points[i]);
                    new_alive.push_back(1);
                }
                else
                {
                    ++total_removed;
                }
            }

            node.points.swap(new_points);
            node.alive.swap(new_alive);
            node.alive_count_ = static_cast<uint32_t>(node.alive.size());
            node.need_compact = false;
            node.dirty_cpu_to_gpu = true;
        }

        if (total_removed > 0)
            version_++;
        return total_removed;
    }

    void PointCloudOctree::markLeavesSynced(std::span<const NodeId> leaf_ids)
    {
        for (NodeId id : leaf_ids)
        {
            if (id == kInvalidNode || id >= nodes_.size())
                continue;
            Node& node = nodes_[id];
            if (!node.active || !node.is_leaf)
                continue;
            node.dirty_cpu_to_gpu = false;
        }
    }

    void PointCloudOctree::markAllLeavesSynced()
    {
        for (Node& node : nodes_)
        {
            if (!node.active || !node.is_leaf) continue;
            node.dirty_cpu_to_gpu = false;
        }
    }

    void PointCloudOctree::markAllLeavesDirty()
    {
        for (Node& node : nodes_)
        {
            if (!node.active || !node.is_leaf) continue;
            node.dirty_cpu_to_gpu = true;
        }
    }

    size_t PointCloudOctree::pruneEmptyLeaves()
    {
        size_t pruned = 0;

        // Repeat until stable: handles chains of empty subtrees at multiple depths.
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (NodeId id = 0; id < static_cast<NodeId>(nodes_.size()); ++id)
            {
                Node& node = nodes_[id];
                if (!node.active || node.is_leaf)
                    continue;

                // Can collapse only if every active child is an empty leaf.
                bool can_collapse = true;
                for (int i = 0; i < 8; ++i)
                {
                    NodeId child_id = node.children[i];
                    if (child_id == kInvalidNode)
                        continue;
                    const Node& child = nodes_[child_id];
                    if (!child.active || !child.is_leaf || child.aliveCount() > 0)
                    {
                        can_collapse = false;
                        break;
                    }
                }

                if (!can_collapse)
                    continue;

                // Orphan all children and promote this node back to a leaf.
                // Push freed slots into the free-list so allocateNode() can recycle them (P1-C).
                for (int i = 0; i < 8; ++i)
                {
                    NodeId child_id = node.children[i];
                    if (child_id != kInvalidNode) {
                        nodes_[child_id].active = false;
                        node_free_list_.push_back(child_id);
                    }
                    node.children[i] = kInvalidNode;
                }
                node.is_leaf = true;
                // points/alive remain empty — GPU sync will destroy the orphaned chunk.
                node.dirty_cpu_to_gpu = true;
                ++pruned;
                changed = true;
            }
        }

        if (pruned > 0)
            version_++;
        return pruned;
    }

    PointCloudOctree::LODTraversalResult PointCloudOctree::traverseForLOD(
        const float* view_proj,
        const float* camera_pos,
        float screen_width,
        float pixel_error_threshold) const
    {
        LODTraversalResult result;
        if (root_ == kInvalidNode)
            return result;

        Frustum frustum;
        extractFrustum(view_proj, frustum);

        result.visible_nodes.reserve(256);
        traverseLODRecursive(root_, frustum, view_proj, camera_pos, screen_width,
                             pixel_error_threshold, result);
        return result;
    }

    PointCloudOctree::Snapshot PointCloudOctree::captureSnapshot(bool skip_empty_leaves, bool dirty_only) const
    {
        Snapshot snap;
        snap.version = version_;

        if (root_ == kInvalidNode)
            return snap;

        snap.leaves.reserve(nodes_.size());

        for (NodeId id = 0; id < static_cast<NodeId>(nodes_.size()); ++id)
        {
            const Node& node = nodes_[id];
            if (!node.active || !node.is_leaf)
                continue;

            const uint32_t alive_count = node.alive_count_;

            if (skip_empty_leaves && alive_count == 0)
                continue;

            LeafView view;
            view.id               = id;
            view.bounds           = node.bounds;
            view.dirty_cpu_to_gpu = node.dirty_cpu_to_gpu;
            view.cached_alive_count = alive_count;

            if (dirty_only && !node.dirty_cpu_to_gpu)
            {
                // T2-5: non-dirty leaf → metadata-only stub, skip expensive deep copy.
                // Explicitly clear data_valid (it defaults to true in LeafView).
                view.data_valid = false;
            }
            else
            {
                view.data_valid = true;
                view.points     = node.points;   // deep copy
                view.alive      = node.alive;    // deep copy
            }

            snap.leaves.push_back(std::move(view));
        }

        return snap;
    }

    uint64_t PointCloudOctree::totalAlivePoints() const
    {
        uint64_t total = 0;
        for (const Node& node : nodes_)
        {
            if (!node.is_leaf || !node.active) continue;
            total += node.alive_count_;
        }
        return total;
    }

    uint32_t PointCloudOctree::leafCount() const
    {
        uint32_t count = 0;
        for (const Node& node : nodes_)
            if (node.is_leaf && !node.points.empty()) ++count;
        return count;
    }

    uint32_t PointCloudOctree::nodeCount() const
    {
        return static_cast<uint32_t>(nodes_.size());
    }

    float PointCloudOctree::computeNodeScreenError(
        const AABB& bounds,
        const float* view_proj,
        const float* camera_pos,
        float screen_width)
    {
        return lux::render::computeScreenError(
            bounds.min.data(), bounds.max.data(),
            view_proj, camera_pos, screen_width);
    }

    void PointCloudOctree::traverseLODRecursive(
        NodeId node_id,
        const Frustum& frustum,
        const float* view_proj,
        const float* camera_pos,
        float screen_width,
        float pixel_error_threshold,
        LODTraversalResult& result) const
    {
        const Node& node = nodes_[node_id];

        if (!frustum.isAABBInside(node.bounds.min, node.bounds.max))
            return;

        float screen_error = computeNodeScreenError(node.bounds, view_proj, camera_pos, screen_width);

        if (screen_error < pixel_error_threshold)
        {
            if (node.is_leaf && !node.points.empty())
                result.visible_nodes.push_back(node_id);

            if (!node.is_leaf)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (node.children[i] != kInvalidNode)
                        result.to_merge.push_back(node.children[i]);
                }
            }
            return;
        }

        if (node.is_leaf)
        {
            if (!node.points.empty())
                result.visible_nodes.push_back(node_id);
            result.to_split.push_back(node_id);
            return;
        }

        bool has_any_child = false;
        for (int i = 0; i < 8; ++i)
        {
            if (node.children[i] != kInvalidNode)
            {
                has_any_child = true;
                traverseLODRecursive(node.children[i], frustum, view_proj, camera_pos,
                                     screen_width, pixel_error_threshold, result);
            }
        }

        if (!has_any_child)
            result.visible_nodes.push_back(node_id);
    }

    bool PointCloudOctree::hasAnyChild(NodeId node_id) const
    {
        const Node& node = nodes_[node_id];
        for (int i = 0; i < 8; ++i)
            if (node.children[i] != kInvalidNode) return true;
        return false;
    }

    PointCloudOctree::NodeId PointCloudOctree::allocateNode()
    {
        // Reuse an orphaned slot from pruneEmptyLeaves() before growing the vector (P1-C).
        if (!node_free_list_.empty()) {
            NodeId id = node_free_list_.back();
            node_free_list_.pop_back();
            nodes_[id] = Node{};   // full reset: clears children/flags/points/alive
            return id;
        }
        NodeId id = static_cast<NodeId>(nodes_.size());
        nodes_.emplace_back();
        return id;
    }

    AABB PointCloudOctree::computeChildBounds(const AABB& parent, int child_index)
    {
        Eigen::Vector3f c = parent.center();
        AABB r;

        for (int axis = 0; axis < 3; ++axis)
        {
            bool positive = (child_index & (1 << axis)) != 0;
            r.min[axis] = positive ? c[axis] : parent.min[axis];
            r.max[axis] = positive ? parent.max[axis] : c[axis];
        }

        return r;
    }

    int PointCloudOctree::childIndexForPosition(const AABB& bounds, const Eigen::Vector3f& pos)
    {
        Eigen::Vector3f c = bounds.center();
        int idx = 0;
        if (pos.x() >= c.x()) idx |= 1;
        if (pos.y() >= c.y()) idx |= 2;
        if (pos.z() >= c.z()) idx |= 4;
        return idx;
    }

    void PointCloudOctree::insertPointInternal(NodeId node_id, const Point& p, uint32_t depth)
    {
        Node& node = nodes_[node_id];

        if (!node.bounds.contains(p.position))
        {
            return;
        }

        if (node.is_leaf)
        {
            node.points.push_back(p);
            node.alive.push_back(1);
            ++node.alive_count_;
            node.dirty_cpu_to_gpu = true;

            if (node.points.size() > config_.max_points_per_leaf && depth < config_.max_depth)
            {
                splitLeaf(node_id, depth);
            }

            return;
        }
        else
        {
            int child_idx = childIndexForPosition(node.bounds, p.position);
            NodeId child_id = node.children[child_idx];
            if (child_id == kInvalidNode)
            {
                child_id = allocateNode();
                nodes_[node_id].children[child_idx] = child_id;
                Node& child = nodes_[child_id];
                child.bounds = computeChildBounds(nodes_[node_id].bounds, child_idx);
                child.is_leaf = true;
                child.depth = depth + 1;
            }

            insertPointInternal(child_id, p, depth + 1);
        }
    }

    void PointCloudOctree::splitLeaf(NodeId node_id, uint32_t depth)
    {
        if (!nodes_[node_id].is_leaf)
            return;
        if (depth >= config_.max_depth)
            return;

        nodes_.reserve(nodes_.size() + 8);
        Node& node = nodes_[node_id];

        for (int i = 0; i < 8; ++i)
        {
            NodeId child_id = node.children[i];
            if (child_id == kInvalidNode)
            {
                child_id = allocateNode();
                node.children[i] = child_id;
                Node& child = nodes_[child_id];
                child.bounds = computeChildBounds(node.bounds, i);
                child.is_leaf = true;
                child.depth = depth + 1;
            }
        }

        for (size_t i = 0; i < node.points.size(); ++i)
        {
            if (!node.alive[i])
                continue;

            const Point& p = node.points[i];
            int child_idx = childIndexForPosition(node.bounds, p.position);
            NodeId child_id = node.children[child_idx];
            Node& child = nodes_[child_id];

            child.points.push_back(p);
            child.alive.push_back(1);
            ++child.alive_count_;
            child.dirty_cpu_to_gpu = true;
        }

        uint64_t children_total = 0;
        for (int i = 0; i < 8; ++i)
        {
            for (uint8_t a : nodes_[node.children[i]].alive)
                if (a) ++children_total;
        }
        uint64_t parent_alive = 0;
        for (uint8_t a : node.alive)
            if (a) ++parent_alive;
        if (children_total != parent_alive)
        {
            assert(false && "OCTREE BUG: splitLeaf data loss");
        }

        node.points.clear();
        node.alive.clear();
        node.alive_count_ = 0;
        node.is_leaf = false;
        node.dirty_cpu_to_gpu = false;
    }

} // namespace lux::render::pc
