#include <lux/engine/world/WorldSpatialIndexImpl.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace lux::world
{
    namespace
    {
        using Bounds = WorldSpatialIndex::Bounds;
        constexpr auto kInvalid = error::errorId("lux.world.spatial.invalid");
        constexpr auto kCapacity = error::errorId("lux.world.spatial.capacity");

        bool validBounds(const Bounds& bounds) noexcept
        {
            return math::isFinite(bounds.minimum) && math::isFinite(bounds.maximum) &&
                bounds.minimum.x < bounds.maximum.x && bounds.minimum.y < bounds.maximum.y &&
                bounds.minimum.z < bounds.maximum.z;
        }

        Bounds united(const Bounds& a, const Bounds& b) noexcept
        {
            return {
                {std::min(a.minimum.x, b.minimum.x), std::min(a.minimum.y, b.minimum.y),
                 std::min(a.minimum.z, b.minimum.z)},
                {std::max(a.maximum.x, b.maximum.x), std::max(a.maximum.y, b.maximum.y),
                 std::max(a.maximum.z, b.maximum.z)}
            };
        }

        Bounds expanded(const Bounds& value, double margin) noexcept
        {
            return {
                {value.minimum.x - margin, value.minimum.y - margin, value.minimum.z - margin},
                {value.maximum.x + margin, value.maximum.y + margin, value.maximum.z + margin}
            };
        }

        bool contains(const Bounds& a, const Bounds& b) noexcept
        {
            return a.minimum.x <= b.minimum.x && a.minimum.y <= b.minimum.y && a.minimum.z <= b.minimum.z &&
                a.maximum.x >= b.maximum.x && a.maximum.y >= b.maximum.y && a.maximum.z >= b.maximum.z;
        }

        bool overlaps(const Bounds& a, const Bounds& b) noexcept
        {
            return a.minimum.x < b.maximum.x && a.maximum.x > b.minimum.x &&
                a.minimum.y < b.maximum.y && a.maximum.y > b.minimum.y &&
                a.minimum.z < b.maximum.z && a.maximum.z > b.minimum.z;
        }

        double area(const Bounds& value, double scale) noexcept
        {
            // One common scale per insertion preserves relative SAH costs without overflowing at huge coordinates.
            const auto x = value.maximum.x / scale - value.minimum.x / scale;
            const auto y = value.maximum.y / scale - value.minimum.y / scale;
            const auto z = value.maximum.z / scale - value.minimum.z / scale;
            return 2 * (x * y + y * z + x * z);
        }
    }

    std::uint32_t WorldSpatialIndex::Impl::allocateNode() noexcept
    {
        std::uint32_t node;
        if (free_nodes.empty())
        {
            node = static_cast<std::uint32_t>(node_bounds.size());
            node_bounds.emplace_back();
            parents.push_back(absent);
            left.push_back(absent);
            right.push_back(absent);
            record_indices.push_back(absent);
            heights.push_back(0);
        }
        else
        {
            node = free_nodes.back();
            free_nodes.pop_back();
            parents[node] = left[node] = right[node] = record_indices[node] = absent;
            heights[node] = 0;
        }
        return node;
    }

    void WorldSpatialIndex::Impl::releaseNode(std::uint32_t node) noexcept
    {
        free_nodes.push_back(node);
    }

    void WorldSpatialIndex::Impl::refit(std::uint32_t node) noexcept
    {
        if (left[node] == absent) return;
        node_bounds[node] = united(node_bounds[left[node]], node_bounds[right[node]]);
        heights[node] = 1 + std::max(heights[left[node]], heights[right[node]]);
    }

    std::uint32_t WorldSpatialIndex::Impl::balance(std::uint32_t node) noexcept
    {
        if (left[node] == absent || heights[node] < 2) return node;
        const auto a = left[node];
        const auto b = right[node];
        const auto difference = static_cast<std::int64_t>(heights[b]) - heights[a];
        if (difference >= -1 && difference <= 1) return node;
        const bool rotate_left = difference > 1;
        const auto promoted = rotate_left ? b : a;
        const auto first = left[promoted];
        const auto second = right[promoted];
        const auto tall = heights[first] > heights[second] ? first : second;
        const auto short_child = tall == first ? second : first;
        const auto parent = parents[node];
        parents[promoted] = parent;
        if (parent == absent) root = promoted;
        else if (left[parent] == node) left[parent] = promoted;
        else right[parent] = promoted;
        parents[node] = promoted;
        parents[short_child] = node;
        parents[tall] = promoted;
        if (rotate_left)
        {
            left[promoted] = node;
            right[promoted] = tall;
            right[node] = short_child;
        }
        else
        {
            right[promoted] = node;
            left[promoted] = tall;
            left[node] = short_child;
        }
        refit(node);
        refit(promoted);
        return promoted;
    }

    void WorldSpatialIndex::Impl::insert(std::uint32_t leaf) noexcept
    {
        if (root == absent) { root = leaf; parents[leaf] = absent; return; }
        const auto full = united(node_bounds[root], node_bounds[leaf]);
        const auto scale = std::max({
            std::abs(full.minimum.x), std::abs(full.minimum.y), std::abs(full.minimum.z),
            std::abs(full.maximum.x), std::abs(full.maximum.y), std::abs(full.maximum.z),
            (std::numeric_limits<double>::min)()
        });
        auto sibling = root;
        while (left[sibling] != absent)
        {
            const auto combined = area(united(node_bounds[sibling], node_bounds[leaf]), scale);
            const auto inheritance = 2 * (combined - area(node_bounds[sibling], scale));
            const auto cost = [&](std::uint32_t child) noexcept
            {
                const auto joined = area(united(node_bounds[child], node_bounds[leaf]), scale);
                return inheritance + joined - (left[child] == absent ? 0 : area(node_bounds[child], scale));
            };
            const auto first = cost(left[sibling]);
            const auto second = cost(right[sibling]);
            if (2 * combined < first && 2 * combined < second) break;
            sibling = first < second ? left[sibling] : right[sibling];
        }
        const auto previous = parents[sibling];
        const auto parent = allocateNode();
        parents[parent] = previous;
        left[parent] = sibling;
        right[parent] = leaf;
        parents[sibling] = parents[leaf] = parent;
        if (previous == absent) root = parent;
        else if (left[previous] == sibling) left[previous] = parent;
        else right[previous] = parent;
        for (auto current = parent; current != absent; current = parents[current])
        {
            refit(current);
            current = balance(current);
        }
    }

    void WorldSpatialIndex::Impl::detach(std::uint32_t leaf) noexcept
    {
        if (leaf == root) { root = absent; return; }
        const auto parent = parents[leaf];
        const auto grandparent = parents[parent];
        const auto sibling = left[parent] == leaf ? right[parent] : left[parent];
        parents[sibling] = grandparent;
        if (grandparent == absent) root = sibling;
        else if (left[grandparent] == parent) left[grandparent] = sibling;
        else right[grandparent] = sibling;
        parents[leaf] = absent;
        releaseNode(parent);
        for (auto current = grandparent; current != absent; current = parents[current])
        {
            refit(current);
            current = balance(current);
        }
    }

    WorldSpatialIndex::WorldSpatialIndex() noexcept : impl_(std::make_unique<Impl>())
    {
        constexpr error::ErrorDescriptor errors[]{
            {"lux.world.spatial.invalid", "Invalid spatial batch or bounds"},
            {"lux.world.spatial.capacity", "Spatial node index capacity exhausted"}
        };
        if (!error::ErrorRegistry::instance().registerTypes(errors)) std::terminate();
    }

    WorldSpatialIndex::~WorldSpatialIndex() noexcept = default;

    WorldSpatialIndex::Result
    WorldSpatialIndex::updateBatch(
        std::span<const ecs::Entity> entities,
        std::span<const Bounds> bounds,
        double coverage_margin,
        std::vector<ecs::Entity>& coverage_changed
    ) noexcept
    {
        const bool invalid_margin = !std::isfinite(coverage_margin) || coverage_margin < 0 ||
            coverage_margin > (std::numeric_limits<double>::max)() / 4;
        if (entities.size() != bounds.size() || invalid_margin) return cxx::unexpected(error::Error{kInvalid});
        impl_->input_entities.clear();
        std::uint64_t added = 0;
        for (std::size_t i = 0; i < entities.size(); ++i)
        {
            const auto found = impl_->leaves.find(entities[i]);
            const auto leaf = found == impl_->leaves.end() ? Impl::absent : found->second;
            const bool invalid_entity = entities[i] == ecs::NullEntity ||
                !impl_->input_entities.emplace(entities[i], std::pair{i, leaf}).second;
            const bool invalid_bounds = !validBounds(bounds[i]) || !validBounds(expanded(bounds[i], coverage_margin)) ||
                !validBounds(expanded(bounds[i], 4 * coverage_margin));
            if (invalid_entity || invalid_bounds) return cxx::unexpected(error::Error{kInvalid});
            added += leaf == Impl::absent;
        }
        const auto required = 2 * added - (impl_->records.empty() && added ? 1 : 0);
        const auto available = std::uint64_t{UINT32_MAX} - impl_->node_bounds.size() + impl_->free_nodes.size();
        if (required > available) return cxx::unexpected(error::Error{kCapacity});
        for (const auto& [entity, input] : impl_->input_entities)
        {
            const auto [i, existing_leaf] = input;
            auto leaf = existing_leaf;
            if (leaf != Impl::absent)
            {
                auto& record = impl_->records[impl_->record_indices[leaf]];
                record.actual_bounds = bounds[i];
                const bool retained = contains(record.coverage_bounds, bounds[i]) &&
                    contains(expanded(bounds[i], 4 * coverage_margin), record.coverage_bounds);
                if (retained) continue;
                impl_->detach(leaf);
                record.coverage_bounds = expanded(bounds[i], coverage_margin);
            }
            else
            {
                leaf = impl_->allocateNode();
                impl_->record_indices[leaf] = static_cast<std::uint32_t>(impl_->records.size());
                impl_->records.push_back({entities[i], bounds[i], expanded(bounds[i], coverage_margin)});
                impl_->leaves.emplace(entities[i], leaf);
            }
            impl_->node_bounds[leaf] = impl_->records[impl_->record_indices[leaf]].coverage_bounds;
            impl_->insert(leaf);
            coverage_changed.push_back(entities[i]);
        }
        return {};
    }

    void
    WorldSpatialIndex::removeBatch(
        std::span<const ecs::Entity> entities,
        std::vector<ecs::Entity>& coverage_changed
    ) noexcept
    {
        for (const auto entity : entities)
        {
            auto found = impl_->leaves.find(entity);
            if (found == impl_->leaves.end()) continue;
            const auto leaf = found->second;
            impl_->detach(leaf);
            const auto index = impl_->record_indices[leaf];
            if (index + 1 != impl_->records.size())
            {
                impl_->records[index] = std::move(impl_->records.back());
                impl_->record_indices[impl_->leaves.at(impl_->records[index].entity)] = index;
            }
            impl_->records.pop_back();
            impl_->leaves.erase(entity);
            impl_->releaseNode(leaf);
            coverage_changed.push_back(entity);
        }
    }

    WorldSpatialIndex::Result
    WorldSpatialIndex::queryBatch(
        std::span<const Bounds> regions,
        std::vector<std::size_t>& offsets,
        std::vector<ecs::Entity>& objects
    ) const noexcept
    {
        if (!std::ranges::all_of(regions, validBounds)) return cxx::unexpected(error::Error{kInvalid});
        offsets.clear();
        objects.clear();
        offsets.push_back(0);
        for (const auto& region : regions)
        {
            impl_->query_stack.clear();
            if (impl_->root != Impl::absent) impl_->query_stack.push_back(impl_->root);
            while (!impl_->query_stack.empty())
            {
                const auto node = impl_->query_stack.back();
                impl_->query_stack.pop_back();
                if (!overlaps(impl_->node_bounds[node], region)) continue;
                if (impl_->left[node] == Impl::absent)
                    objects.push_back(impl_->records[impl_->record_indices[node]].entity);
                else
                {
                    impl_->query_stack.push_back(impl_->left[node]);
                    impl_->query_stack.push_back(impl_->right[node]);
                }
            }
            offsets.push_back(objects.size());
        }
        return {};
    }

    const WorldSpatialIndex::Bounds* WorldSpatialIndex::actualBounds(ecs::Entity entity) const noexcept
    {
        const auto found = impl_->leaves.find(entity);
        if (found == impl_->leaves.end()) return nullptr;
        return &impl_->records[impl_->record_indices[found->second]].actual_bounds;
    }

    const WorldSpatialIndex::Bounds* WorldSpatialIndex::coverageBounds(ecs::Entity entity) const noexcept
    {
        const auto found = impl_->leaves.find(entity);
        if (found == impl_->leaves.end()) return nullptr;
        return &impl_->records[impl_->record_indices[found->second]].coverage_bounds;
    }
}
