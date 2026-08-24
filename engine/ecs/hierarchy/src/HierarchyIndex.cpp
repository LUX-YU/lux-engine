#include <lux/engine/ecs/HierarchyIndex.hpp>

#include <lux/engine/ecs/core/detail/WorldAccess.hpp>

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace lux::ecs
{
    namespace
    {
        [[nodiscard]] bool lessEntity(Entity left, Entity right) noexcept
        {
            return entityBits(left) < entityBits(right);
        }
    }

    HierarchyIndex::HierarchyIndex(World& world) noexcept
        : world_(std::addressof(world))
    {}

    lux::cxx::expected<void, EHierarchyError>
    HierarchyIndex::rebuild() noexcept
    {
        try
        {
            const World& world = *world_;
            std::vector<std::pair<Entity, Entity>> next;
            for (auto [child, value] : world.query<Read<Parent>>())
            {
                if (value.entity == NullEntity)
                    continue;
                if (!world.valid(value.entity))
                    return lux::cxx::unexpected(EHierarchyError::INVALID_PARENT);
                if (child == value.entity)
                    return lux::cxx::unexpected(EHierarchyError::SELF_PARENT);
                next.emplace_back(child, value.entity);
            }
            std::sort(
                next.begin(),
                next.end(),
                [](const auto& left, const auto& right)
                {
                    return lessEntity(left.first, right.first);
                }
            );
            parents_ = std::move(next);
            return rebuildOrder();
        }
        catch (...)
        {
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }

    Entity HierarchyIndex::parent(Entity entity) const noexcept
    {
        const auto iterator = std::lower_bound(
            parents_.begin(),
            parents_.end(),
            entity,
            [](const auto& edge, Entity value)
            {
                return lessEntity(edge.first, value);
            }
        );
        return iterator != parents_.end() && iterator->first == entity
            ? iterator->second
            : NullEntity;
    }

    std::span<const Entity> HierarchyIndex::subtree(Entity entity) const noexcept
    {
        const auto iterator = std::lower_bound(
            intervals_.begin(),
            intervals_.end(),
            entity,
            [](const Interval& interval, Entity value)
            {
                return lessEntity(interval.entity, value);
            }
        );
        if (iterator == intervals_.end() || iterator->entity != entity)
            return {};
        return std::span<const Entity>{preorder_}.subspan(
            iterator->begin,
            iterator->end - iterator->begin
        );
    }

    std::span<const Entity> HierarchyIndex::preorder() const noexcept
    {
        return preorder_;
    }

    bool HierarchyIndex::canSetParent(Entity child, Entity next_parent) const noexcept
    {
        if (child == NullEntity || next_parent == NullEntity || child == next_parent)
            return false;
        Entity current = next_parent;
        for (std::size_t depth{}; current != NullEntity && depth <= parents_.size(); ++depth)
        {
            if (current == child)
                return false;
            current = parent(current);
        }
        return current == NullEntity;
    }

    std::size_t HierarchyIndex::size() const noexcept
    {
        return preorder_.size();
    }

    void HierarchyIndex::clear() noexcept
    {
        parents_.clear();
        preorder_.clear();
        intervals_.clear();
    }

    lux::cxx::expected<void, EHierarchyError>
    HierarchyIndex::setEdge(Entity child, Entity next_parent) noexcept
    {
        if (!canSetParent(child, next_parent))
            return lux::cxx::unexpected(
                child == next_parent ? EHierarchyError::SELF_PARENT : EHierarchyError::CYCLE
            );
        try
        {
            const auto iterator = std::lower_bound(
                parents_.begin(),
                parents_.end(),
                child,
                [](const auto& edge, Entity value)
                {
                    return lessEntity(edge.first, value);
                }
            );
            if (iterator != parents_.end() && iterator->first == child)
                iterator->second = next_parent;
            else
                parents_.insert(iterator, {child, next_parent});
            return rebuildOrder();
        }
        catch (...)
        {
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }

    void HierarchyIndex::eraseEdge(Entity child) noexcept
    {
        const auto iterator = std::lower_bound(
            parents_.begin(),
            parents_.end(),
            child,
            [](const auto& edge, Entity value)
            {
                return lessEntity(edge.first, value);
            }
        );
        if (iterator != parents_.end() && iterator->first == child)
            parents_.erase(iterator);
        if (!rebuildOrder())
            detail::contractFailure();
    }

    lux::cxx::expected<void, EHierarchyError>
    HierarchyIndex::rebuildOrder() noexcept
    {
        try
        {
            std::vector<Entity> nodes;
            nodes.reserve(parents_.size() * 2U);
            for (const auto [child, parent_entity] : parents_)
            {
                nodes.push_back(child);
                nodes.push_back(parent_entity);
            }
            std::sort(nodes.begin(), nodes.end(), lessEntity);
            nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());

            std::unordered_map<Entity, std::vector<Entity>> children;
            children.reserve(nodes.size());
            for (const auto [child, parent_entity] : parents_)
                children[parent_entity].push_back(child);
            for (auto& [parent_entity, values] : children)
                std::sort(values.begin(), values.end(), lessEntity);

            std::unordered_set<Entity> visiting;
            std::unordered_set<Entity> visited;
            std::vector<Interval> intervals;
            std::vector<Entity> order;
            order.reserve(nodes.size());
            intervals.reserve(nodes.size());

            std::function<bool(Entity)> visit = [&](Entity entity)
            {
                if (visiting.contains(entity))
                    return false;
                if (visited.contains(entity))
                    return true;
                visiting.insert(entity);
                const auto begin = static_cast<std::uint32_t>(order.size());
                order.push_back(entity);
                if (const auto iterator = children.find(entity); iterator != children.end())
                {
                    for (const Entity child : iterator->second)
                    {
                        if (!visit(child))
                            return false;
                    }
                }
                visiting.erase(entity);
                visited.insert(entity);
                intervals.push_back(Interval{
                    entity,
                    begin,
                    static_cast<std::uint32_t>(order.size())});
                return true;
            };

            for (const Entity entity : nodes)
            {
                if (parent(entity) == NullEntity && !visit(entity))
                    return lux::cxx::unexpected(EHierarchyError::CYCLE);
            }
            for (const Entity entity : nodes)
            {
                if (!visit(entity))
                    return lux::cxx::unexpected(EHierarchyError::CYCLE);
            }
            std::sort(
                intervals.begin(),
                intervals.end(),
                [](const Interval& left, const Interval& right)
                {
                    return lessEntity(left.entity, right.entity);
                }
            );
            preorder_ = std::move(order);
            intervals_ = std::move(intervals);
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<void, EHierarchyError> setParent(
        WorldEdit& edit,
        HierarchyIndex& hierarchy,
        Entity child,
        Entity parent_entity
    ) noexcept
    {
        World& world = detail::WorldEditAccess::world(edit);
        if (!hierarchy.boundTo(world))
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);
        if (!world.valid(child) || !world.valid(parent_entity))
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);
        if (child == parent_entity)
            return lux::cxx::unexpected(EHierarchyError::SELF_PARENT);
        if (!hierarchy.canSetParent(child, parent_entity))
            return lux::cxx::unexpected(EHierarchyError::CYCLE);

        const auto indexed = hierarchy.setEdge(child, parent_entity);
        if (!indexed)
            return indexed;
        if (world.find<Parent>(child) != nullptr)
        {
            edit.update<Parent>(child, [parent_entity](Parent& value) noexcept
            {
                value.entity = parent_entity;
            });
        }
        else
            edit.emplace<Parent>(child, parent_entity);
        return {};
    }

    lux::cxx::expected<void, EHierarchyError> clearParent(
        WorldEdit& edit,
        HierarchyIndex& hierarchy,
        Entity child
    ) noexcept
    {
        World& world = detail::WorldEditAccess::world(edit);
        if (!hierarchy.boundTo(world))
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);
        if (!world.valid(child))
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);
        hierarchy.eraseEdge(child);
        if (world.find<Parent>(child) != nullptr)
            edit.erase<Parent>(child);
        return {};
    }
} // namespace lux::ecs
