#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <entt/signal/sigh.hpp>

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

    QueryResult<std::shared_ptr<const MeshQueryGeometry>>
    MeshQueryGeometry::build(std::span<const Eigen::Vector3f> positions, std::span<const std::uint32_t> indices)
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
        geometry->triangles.build(positions.data(), static_cast<std::uint32_t>(positions.size()), indices.data(),
                                  static_cast<std::uint32_t>(indices.size()));
        return std::shared_ptr<const MeshQueryGeometry>(std::move(geometry));
    }

    struct MeshQuerySystem::Impl
    {
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

        explicit Impl(Registry &value)
            : registry(value), mesh_added(registry.on_construct<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              mesh_changed(registry.on_update<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              mesh_removed(registry.on_destroy<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              pose_added(registry.on_construct<WorldTransform3D>().connect<&Impl::structureChanged>(*this)),
              pose_changed(registry.on_update<WorldTransform3D>().connect<&Impl::transformChanged>(*this)),
              pose_removed(registry.on_destroy<WorldTransform3D>().connect<&Impl::structureChanged>(*this))
        {
        }

        void structureChanged(Registry &, Entity) noexcept
        {
            structure_dirty = true;
        }

        void transformChanged(Registry &, Entity entity)
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

        bool updateLeaf(Leaf &leaf)
        {
            leaf.dirty = false;
            const auto &world = registry.get<WorldTransform3D>(leaf.entity).value;
            const Eigen::FullPivLU<Eigen::Matrix3d> decomposition{world.linear()};
            if (!world.matrix().allFinite() || !decomposition.isInvertible())
            {
                ready = lux::cxx::unexpected(MeshQueryFailure{EMeshQueryError::INVALID_TRANSFORM, leaf.entity,
                                                              registry.get<Mesh3D>(leaf.entity).value.mesh});
                return false;
            }

            leaf.inverse = decomposition.inverse();
            leaf.translation = world.translation();
            const auto &local = leaf.geometry->bounds;
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
            std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end,
                             [&](auto left, auto right)
                             { return leaves[left].bounds.center()[axis] < leaves[right].bounds.center()[axis]; });
            const auto left = buildNode(begin, middle, index);
            const auto right = buildNode(middle, end, index);
            nodes[index].left = left;
            nodes[index].right = right;
            return index;
        }

        void rebuild()
        {
            leaves.clear();
            nodes.clear();
            order.clear();
            changed.clear();
            entity_leaf.clear();
            ready = {};

            for (const Entity entity : registry.view<const Mesh3D>())
            {
                const auto &visual = registry.get<Mesh3D>(entity).value;
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
                if (updateLeaf(leaf))
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

        void update()
        {
            if (structure_dirty || (!ready && !changed.empty()))
            {
                rebuild();
                return;
            }
            for (const auto index : changed)
            {
                auto &leaf = leaves[index];
                if (!updateLeaf(leaf))
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

        static bool intersects(const Eigen::AlignedBox3d &bounds, const math::Ray3d &ray, double maximum,
                               double &near) noexcept
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

        void query(std::uint32_t node_index, const math::Ray3d &ray, RayHit3D &best, bool &found,
                   MeshQueryWork &work) const
        {
            ++work.tree_nodes;
            const auto &node = nodes[node_index];
            double near{};
            if (!intersects(node.bounds, ray, best.distance, near))
            {
                return;
            }
            if (node.leaf != None)
            {
                ++work.candidates;
                const auto &leaf = leaves[node.leaf];
                const Eigen::Vector3d direction = leaf.inverse * ray.direction;
                const double scale = direction.norm();
                if (!std::isfinite(scale) || scale <= 0.0)
                {
                    return;
                }
                const math::Ray local{(leaf.inverse * (ray.origin - leaf.translation)).cast<float>(),
                                      (direction / scale).cast<float>()};
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
                best = {leaf.entity, ray.pointAt(distance),
                        (leaf.inverse.transpose() * hit->normal.cast<double>()).normalized(), distance,
                        hit->triangle_index};
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

        Registry &registry;
        std::unordered_map<asset::AssetId, QueryResult<std::shared_ptr<const MeshQueryGeometry>>> geometries;
        std::unordered_map<Entity, std::uint32_t> entity_leaf;
        std::vector<Leaf> leaves;
        std::vector<Node> nodes;
        std::vector<std::uint32_t> order, changed;
        QueryResult<void> ready;
        bool structure_dirty{true};
        std::uint64_t rebuilds{}, leaf_updates{};
        entt::scoped_connection mesh_added, mesh_changed, mesh_removed;
        entt::scoped_connection pose_added, pose_changed, pose_removed;
    };

    MeshQuerySystem::MeshQuerySystem(Registry &registry) : impl_(std::make_unique<Impl>(registry)) {}
    MeshQuerySystem::~MeshQuerySystem() noexcept = default;

    void MeshQuerySystem::setGeometry(asset::AssetId source, std::shared_ptr<const MeshQueryGeometry> geometry)
    {
        if (!geometry)
        {
            removeGeometry(source);
            return;
        }
        const auto current = impl_->geometries.find(source);
        if (current != impl_->geometries.end() && current->second && *current->second == geometry)
        {
            return;
        }
        impl_->geometries.insert_or_assign(source, std::move(geometry));
        impl_->structure_dirty = true;
    }

    void MeshQuerySystem::removeGeometry(asset::AssetId source)
    {
        if (impl_->geometries.erase(source) != 0)
        {
            impl_->structure_dirty = true;
        }
    }

    void MeshQuerySystem::setGeometryFailure(asset::AssetId source, MeshQueryFailure failure)
    {
        failure.mesh = source;
        const auto current = impl_->geometries.find(source);
        if (current != impl_->geometries.end() && !current->second && current->second.error().code == failure.code)
        {
            return;
        }
        impl_->geometries.insert_or_assign(source, lux::cxx::unexpected(failure));
        impl_->structure_dirty = true;
    }

    void MeshQuerySystem::updateStablePoint()
    {
        impl_->update();
    }

    bool MeshQuerySystem::hasPendingChanges() const noexcept
    {
        return impl_->structure_dirty || !impl_->changed.empty();
    }

    QueryResult<bool> MeshQuerySystem::raycastNearest(const math::Ray3d &ray, double maximum_distance, RayHit3D &hit,
                                                      MeshQueryWork *work) const
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
        if (impl_->structure_dirty || !impl_->changed.empty())
        {
            return lux::cxx::unexpected(MeshQueryFailure{EMeshQueryError::NOT_READY});
        }
        if (!impl_->ready)
        {
            return lux::cxx::unexpected(impl_->ready.error());
        }

        MeshQueryWork actual;
        RayHit3D best;
        best.distance = maximum_distance;
        bool found{};
        if (!impl_->nodes.empty())
        {
            impl_->query(0, ray, best, found, actual);
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

    MeshQueryStatistics MeshQuerySystem::statistics() const noexcept
    {
        const auto &data = *impl_;
        MeshQueryStatistics result{data.leaves.size(),
                                   0,
                                   data.nodes.size(),
                                   sizeof(Impl) + data.leaves.capacity() * sizeof(Impl::Leaf) +
                                       data.nodes.capacity() * sizeof(Impl::Node) +
                                       (data.order.capacity() + data.changed.capacity()) * sizeof(std::uint32_t) +
                                       data.entity_leaf.size() * sizeof(decltype(data.entity_leaf)::value_type) +
                                       data.geometries.size() * sizeof(decltype(data.geometries)::value_type),
                                   data.rebuilds,
                                   data.leaf_updates};
        for (const auto &[asset, geometry] : data.geometries)
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

    SceneSystemRegistration builtinMeshQuerySystemRegistration() noexcept
    {
        return {
            .type = system::systemTypeId(MeshQuerySystem::Description.canonical_name),
            .cpp_type = lux::cxx::typeToken<MeshQuerySystem>(),
            .description = &MeshQuerySystem::Description,
            .install = +[](SceneBuilder &builder,
                           SceneSystemView description) noexcept -> lux::cxx::expected<void, SceneSystemBuildFailure>
            {
                auto instance = builder.emplaceSystem<MeshQuerySystem>(description.instanceId(), builder.registry());
                if (!instance)
                {
                    return lux::cxx::unexpected(instance.error());
                }
                return builder.addStablePointTask<MeshQuerySystem>(
                    description.instanceId(), [](MeshQuerySystem &query) noexcept { query.updateStablePoint(); });
            }};
    }
} // namespace lux::scene
