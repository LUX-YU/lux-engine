#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace lux::scene
{
    using simulation::ecs::Entity;
    using simulation::ecs::Mesh3D;
    using simulation::ecs::Registry;
    using simulation::ecs::WorldTransform3D;

    QueryResult<std::shared_ptr<const MeshQueryGeometry>> MeshQueryGeometry::build(
        std::span<const Eigen::Vector3f> positions,
        std::span<const std::uint32_t> indices
    )
    {
        if (positions.size() > UINT32_MAX || indices.size() > UINT32_MAX || indices.size() % 3 != 0)
        {
            return lux::cxx::unexpected(MeshQueryFailure{EMeshQueryError::INVALID_GEOMETRY});
        }
        for (const auto index : indices)
        {
            if (index >= positions.size() || !positions[index].allFinite())
            {
                return lux::cxx::unexpected(MeshQueryFailure{EMeshQueryError::INVALID_GEOMETRY});
            }
        }

        auto geometry = std::make_shared<MeshQueryGeometry>();
        for (const auto index : indices)
        {
            geometry->bounds.merge(positions[index]);
        }
        geometry->triangles.build(
            positions.data(),
            static_cast<std::uint32_t>(positions.size()),
            indices.data(),
            static_cast<std::uint32_t>(indices.size())
        );
        return std::shared_ptr<const MeshQueryGeometry>(std::move(geometry));
    }

    struct MeshQuery::Impl
    {
        void setGeometry(asset::AssetId, std::shared_ptr<const MeshQueryGeometry>);
        void removeGeometry(asset::AssetId);
        void setGeometryFailure(asset::AssetId, MeshQueryFailure);
        [[nodiscard]] bool hasPendingChanges() const noexcept;
        [[nodiscard]] QueryResult<bool> raycastNearest(const math::Ray3d&, double, RayHit3D&, MeshQueryWork*) const;
        [[nodiscard]] MeshQueryStatistics statistics() const noexcept;

        static constexpr std::uint32_t None = UINT32_MAX;

        struct Leaf
        {
            Entity entity{simulation::ecs::NullEntity};
            std::shared_ptr<const MeshQueryGeometry> geometry;
            Eigen::AlignedBox3d bounds;
            Eigen::Matrix3d inverse;
            Eigen::Vector3d translation;
            std::uint32_t node{None};
            bool dirty{};
        };

        struct Node
        {
            Eigen::AlignedBox3d bounds;
            std::uint32_t parent{None}, left{None}, right{None}, leaf{None};
        };

        void structureChanged() noexcept
        {
            structure_dirty = true;
        }

        void transformChanged(Registry& registry, Entity entity)
        {
            const auto position = entity_leaf.find(entity);
            if (position != entity_leaf.end() && !leaves[position->second].dirty)
            {
                leaves[position->second].dirty = true;
                changed.push_back(position->second);
            }
            else if (position == entity_leaf.end() && registry.all_of<Mesh3D>(entity))
            {
                structure_dirty = true;
            }
        }

        bool updateLeaf(Registry& registry, Leaf& leaf)
        {
            leaf.dirty = false;
            const auto& world = registry.get<WorldTransform3D>(leaf.entity).value;
            const Eigen::FullPivLU<Eigen::Matrix3d> decomposition{world.linear()};
            if (!world.matrix().allFinite() || !decomposition.isInvertible())
            {
                ready = lux::cxx::unexpected(MeshQueryFailure{
                    EMeshQueryError::INVALID_TRANSFORM,
                    leaf.entity,
                    registry.get<Mesh3D>(leaf.entity).value.mesh
                });
                return false;
            }

            leaf.inverse = decomposition.inverse();
            leaf.translation = world.translation();
            const auto& local = leaf.geometry->bounds;
            const Eigen::Vector3d center = (local.min.cast<double>() + local.max.cast<double>()) * 0.5;
            const Eigen::Vector3d half_extent = (local.max.cast<double>() - local.min.cast<double>()) * 0.5;
            const Eigen::Vector3d world_center = world * center;
            const Eigen::Vector3d world_half_extent = world.linear().cwiseAbs() * half_extent;
            leaf.bounds = Eigen::AlignedBox3d{world_center - world_half_extent, world_center + world_half_extent};
            leaf.dirty = false;
            ++leaf_updates;
            return true;
        }

        std::uint32_t buildNode(std::size_t begin, std::size_t end, std::uint32_t parent)
        {
            const auto index = static_cast<std::uint32_t>(nodes.size());
            nodes.emplace_back();
            nodes[index].parent = parent;
            for (std::size_t position = begin; position < end; ++position)
            {
                nodes[index].bounds.extend(leaves[order[position]].bounds);
            }
            if (end - begin == 1)
            {
                nodes[index].leaf = order[begin];
                leaves[order[begin]].node = index;
                return index;
            }

            Eigen::Index axis{};
            nodes[index].bounds.sizes().maxCoeff(&axis);
            const auto middle = begin + (end - begin) / 2;
            std::nth_element(
                order.begin() + begin,
                order.begin() + middle,
                order.begin() + end,
                [&](auto left, auto right) {
                    return leaves[left].bounds.center()[axis] < leaves[right].bounds.center()[axis];
                }
            );
            const auto left = buildNode(begin, middle, index);
            const auto right = buildNode(middle, end, index);
            nodes[index].left = left;
            nodes[index].right = right;
            return index;
        }

        void rebuild(Registry& registry)
        {
            leaves.clear();
            nodes.clear();
            order.clear();
            changed.clear();
            entity_leaf.clear();
            ready = {};

            for (const Entity entity : registry.view<const Mesh3D>())
            {
                const auto& visual = registry.get<Mesh3D>(entity).value;
                if (!visual.visible || visual.mesh.isNull())
                {
                    continue;
                }
                const auto geometry = geometries.find(visual.mesh);
                if (!registry.all_of<WorldTransform3D>(entity) || geometry == geometries.end())
                {
                    ready = lux::cxx::unexpected(MeshQueryFailure{EMeshQueryError::NOT_READY, entity, visual.mesh});
                    continue;
                }
                if (!geometry->second)
                {
                    auto failure = geometry->second.error();
                    failure.entity = entity;
                    ready = lux::cxx::unexpected(failure);
                    continue;
                }
                if (!(*geometry->second)->triangles.isBuilt())
                {
                    continue;
                }

                Leaf leaf;
                leaf.entity = entity;
                leaf.geometry = *geometry->second;
                if (leaves.size() >= UINT32_MAX / 2)
                {
                    ready = lux::cxx::unexpected(MeshQueryFailure{EMeshQueryError::CAPACITY, entity, visual.mesh});
                    break;
                }
                if (updateLeaf(registry, leaf))
                {
                    entity_leaf.emplace(entity, static_cast<std::uint32_t>(leaves.size()));
                    leaves.push_back(std::move(leaf));
                }
            }

            order.resize(leaves.size());
            std::iota(order.begin(), order.end(), 0U);
            nodes.reserve(leaves.empty() ? 0 : leaves.size() * 2 - 1);
            if (!leaves.empty())
            {
                buildNode(0, leaves.size(), None);
            }
            structure_dirty = false;
            ++rebuilds;
        }

        void update(Registry& registry)
        {
            if (structure_dirty || (!ready && !changed.empty()))
            {
                rebuild(registry);
                return;
            }
            for (const auto index : changed)
            {
                auto& leaf = leaves[index];
                if (!updateLeaf(registry, leaf))
                {
                    continue;
                }
                auto node = leaf.node;
                nodes[node].bounds = leaf.bounds;
                while (nodes[node].parent != None)
                {
                    node = nodes[node].parent;
                    nodes[node].bounds = nodes[nodes[node].left].bounds.merged(nodes[nodes[node].right].bounds);
                }
            }
            changed.clear();
        }

        static bool intersects(
            const Eigen::AlignedBox3d& bounds,
            const math::Ray3d& ray,
            double maximum,
            double& near
        ) noexcept
        {
            near = 0.0;
            double far = maximum;
            for (Eigen::Index axis = 0; axis < 3; ++axis)
            {
                const double direction = ray.direction[axis];
                if (direction == 0.0)
                {
                    if (ray.origin[axis] < bounds.min()[axis] || ray.origin[axis] > bounds.max()[axis])
                    {
                        return false;
                    }
                    continue;
                }
                const double first = (bounds.min()[axis] - ray.origin[axis]) / direction;
                const double second = (bounds.max()[axis] - ray.origin[axis]) / direction;
                near = std::max(near, std::min(first, second));
                far = std::min(far, std::max(first, second));
                if (near > far)
                {
                    return false;
                }
            }
            return true;
        }

        void query(std::uint32_t node_index, const math::Ray3d& ray, RayHit3D& best, bool& found, MeshQueryWork& work)
            const
        {
            ++work.tree_nodes;
            const auto& node = nodes[node_index];
            double near{};
            if (!intersects(node.bounds, ray, best.distance, near))
            {
                return;
            }
            if (node.leaf != None)
            {
                ++work.candidates;
                const auto& leaf = leaves[node.leaf];
                const Eigen::Vector3d direction = leaf.inverse * ray.direction;
                const double scale = direction.norm();
                if (!std::isfinite(scale) || scale <= 0.0)
                {
                    return;
                }
                const math::Ray local{
                    (leaf.inverse * (ray.origin - leaf.translation)).cast<float>(),
                    (direction / scale).cast<float>()
                };
                if (!local.origin.allFinite() || !local.direction.allFinite() || scale <= 0.0)
                {
                    return;
                }
                const auto hit = leaf.geometry->triangles.intersectLocal(local);
                if (!hit)
                {
                    return;
                }
                const double distance = hit->t / scale;
                if (distance > best.distance ||
                    (found && distance == best.distance &&
                     simulation::ecs::entityBits(leaf.entity) > simulation::ecs::entityBits(best.entity)))
                {
                    return;
                }
                found = true;
                best = {
                    leaf.entity,
                    ray.pointAt(distance),
                    (leaf.inverse.transpose() * hit->normal.cast<double>()).normalized(),
                    distance,
                    hit->triangle_index
                };
                return;
            }

            double left_distance{}, right_distance{};
            const bool left = intersects(nodes[node.left].bounds, ray, best.distance, left_distance);
            const bool right = intersects(nodes[node.right].bounds, ray, best.distance, right_distance);
            const auto first = right && (!left || right_distance < left_distance) ? node.right : node.left;
            const auto second = first == node.left ? node.right : node.left;
            if (left || right)
            {
                query(first, ray, best, found, work);
            }
            if (left && right)
            {
                query(second, ray, best, found, work);
            }
        }

        std::unordered_map<asset::AssetId, QueryResult<std::shared_ptr<const MeshQueryGeometry>>> geometries;
        std::unordered_map<Entity, std::uint32_t> entity_leaf;
        std::vector<Leaf> leaves;
        std::vector<Node> nodes;
        std::vector<std::uint32_t> order, changed;
        QueryResult<void> ready;
        bool structure_dirty{true};
        std::uint64_t rebuilds{}, leaf_updates{};
    };

    MeshQuery::MeshQuery() : impl_(std::make_unique<Impl>()) {}
    MeshQuery::~MeshQuery() noexcept = default;

    void MeshQuery::Impl::setGeometry(asset::AssetId source, std::shared_ptr<const MeshQueryGeometry> geometry)
    {
        if (!geometry)
        {
            removeGeometry(source);
            return;
        }
        const auto current = geometries.find(source);
        if (current != geometries.end() && current->second && *current->second == geometry)
        {
            return;
        }
        geometries.insert_or_assign(source, std::move(geometry));
        structure_dirty = true;
    }

    void MeshQuery::Impl::removeGeometry(asset::AssetId source)
    {
        if (geometries.erase(source) != 0)
        {
            structure_dirty = true;
        }
    }

    void MeshQuery::Impl::setGeometryFailure(asset::AssetId source, MeshQueryFailure failure)
    {
        failure.mesh = source;
        const auto current = geometries.find(source);
        if (current != geometries.end() && !current->second && current->second.error().code == failure.code)
        {
            return;
        }
        geometries.insert_or_assign(source, lux::cxx::unexpected(failure));
        structure_dirty = true;
    }

    void MeshQuery::update(Registry& registry)
    {
        impl_->update(registry);
    }

    bool MeshQuery::Impl::hasPendingChanges() const noexcept
    {
        return structure_dirty || !changed.empty();
    }

    QueryResult<bool> MeshQuery::Impl::raycastNearest(
        const math::Ray3d& ray,
        double maximum_distance,
        RayHit3D& hit,
        MeshQueryWork* work
    ) const
    {
        if (work)
        {
            *work = {};
        }
        if (!ray.origin.allFinite() || !ray.direction.allFinite() || !std::isfinite(maximum_distance) ||
            maximum_distance <= 0.0 || std::abs(ray.direction.squaredNorm() - 1.0) > 1.0e-8)
        {
            return lux::cxx::unexpected(MeshQueryFailure{EMeshQueryError::INVALID_INPUT});
        }
        if (structure_dirty || !changed.empty())
        {
            return lux::cxx::unexpected(MeshQueryFailure{EMeshQueryError::NOT_READY});
        }
        if (!ready)
        {
            return lux::cxx::unexpected(ready.error());
        }

        MeshQueryWork actual;
        RayHit3D best;
        best.distance = maximum_distance;
        bool found{};
        if (!nodes.empty())
        {
            query(0, ray, best, found, actual);
        }
        if (work)
        {
            *work = actual;
        }
        if (found)
        {
            hit = best;
        }
        return found;
    }

    MeshQueryStatistics MeshQuery::Impl::statistics() const noexcept
    {
        MeshQueryStatistics result{
            leaves.size(),
            0,
            nodes.size(),
            sizeof(Impl) + leaves.capacity() * sizeof(Impl::Leaf) + nodes.capacity() * sizeof(Impl::Node) +
                (order.capacity() + changed.capacity()) * sizeof(std::uint32_t) +
                entity_leaf.size() * sizeof(decltype(entity_leaf)::value_type) +
                geometries.size() * sizeof(decltype(geometries)::value_type),
            rebuilds,
            leaf_updates
        };
        for (const auto& [asset, geometry] : geometries)
        {
            if (geometry)
            {
                ++result.geometries;
                result.geometry_bytes +=
                    sizeof(MeshQueryGeometry) - sizeof(math::MeshBVH) + (*geometry)->triangles.retainedBytes();
            }
        }
        return result;
    }

    void MeshQuery::structureChanged() noexcept
    {
        impl_->structureChanged();
    }
    void MeshQuery::transformChanged(Registry& registry, Entity entity)
    {
        impl_->transformChanged(registry, entity);
    }
    void MeshQuery::setGeometry(asset::AssetId id, std::shared_ptr<const MeshQueryGeometry> geometry)
    {
        impl_->setGeometry(id, std::move(geometry));
    }
    void MeshQuery::removeGeometry(asset::AssetId id)
    {
        impl_->removeGeometry(id);
    }
    void MeshQuery::setGeometryFailure(asset::AssetId id, MeshQueryFailure failure)
    {
        impl_->setGeometryFailure(id, failure);
    }
    bool MeshQuery::hasPendingChanges() const noexcept
    {
        return impl_->hasPendingChanges();
    }
    QueryResult<bool> MeshQuery::raycastNearest(
        const math::Ray3d& ray,
        double distance,
        RayHit3D& hit,
        MeshQueryWork* work
    ) const
    {
        return impl_->raycastNearest(ray, distance, hit, work);
    }
    MeshQueryStatistics MeshQuery::statistics() const noexcept
    {
        return impl_->statistics();
    }
}
