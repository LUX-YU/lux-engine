#pragma once

#include <lux/engine/ecs/ComponentChangeSet.hpp>
#include <lux/engine/ecs/EcsVerify.hpp>
#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace lux::ecs
{
    namespace detail
    {
        template <class Source, class Derived>
        void emplaceDerivedOnSourceConstruct(
            lux::meta::EntityRegistryBase& registry,
            entt::entity entity)
        {
            if (!registry.all_of<Derived>(entity))
                registry.emplace<Derived>(entity);
        }

        template <class Source, class Derived>
        void removeDerivedOnSourceDestroy(
            lux::meta::EntityRegistryBase& registry,
            entt::entity entity)
        {
            registry.remove<Derived>(entity);
            // A destroyed parent makes its children roots even though their
            // ParentComponent values did not change. Invalidate the one shared
            // topology index so the next resolver pass observes that fact.
            if (auto* hierarchy = registry.ctx().find<HierarchyIndex>())
                hierarchy->markTopologyDirty();
        }
    } // namespace detail

    template <class Source, class Derived>
    inline void connectDerivedMaintenance(
        lux::meta::EntityRegistry& registry,
        std::vector<lux::meta::entity_id>& scratch)
    {
        registry.on_construct<Source>()
            .template connect<&detail::emplaceDerivedOnSourceConstruct<Source, Derived>>();
        registry.on_destroy<Source>()
            .template connect<&detail::removeDerivedOnSourceDestroy<Source, Derived>>();

        scratch.clear();
        for (const auto entity : registry.view<Source>(entt::exclude<Derived>))
            scratch.push_back(entity);
        for (const auto entity : scratch)
            registry.emplace<Derived>(entity);

        scratch.clear();
        for (const auto entity : registry.view<Derived>(entt::exclude<Source>))
            scratch.push_back(entity);
        for (const auto entity : scratch)
            registry.remove<Derived>(entity);
    }

    template <class Source, class Derived>
    inline void disconnectDerivedMaintenance(
        lux::meta::EntityRegistry& registry)
    {
        registry.on_construct<Source>()
            .template disconnect<
                &detail::emplaceDerivedOnSourceConstruct<Source, Derived>>();
        registry.on_destroy<Source>()
            .template disconnect<
                &detail::removeDerivedOnSourceDestroy<Source, Derived>>();
    }

    [[nodiscard]] inline bool wouldCreateHierarchyCycle(
        lux::meta::EntityRegistryBase& registry,
        lux::meta::entity_id child,
        lux::meta::entity_id new_parent)
    {
        const auto& storage = registry.storage<ParentComponent>();
        std::size_t budget = storage.size() + 1u;
        for (auto entity = new_parent;
             registry.valid(entity) && budget != 0u;
             --budget)
        {
            if (entity == child)
                return true;
            const auto* parent = registry.try_get<ParentComponent>(entity);
            if (!parent)
                break;
            entity = parent->parent();
        }
        return budget == 0u;
    }

    inline bool setParent(
        lux::meta::EntityRegistryBase& registry,
        lux::meta::entity_id child,
        lux::meta::entity_id new_parent)
    {
        if (child == lux::meta::null_entity || !registry.valid(child))
            return false;
        if (new_parent == lux::meta::null_entity || !registry.valid(new_parent))
        {
            registry.remove<ParentComponent>(child);
            return true;
        }
        if (wouldCreateHierarchyCycle(registry, child, new_parent))
            return false;
        detail::ensureHierarchyMembership(registry, child);
        detail::ensureHierarchyMembership(registry, new_parent);
        registry.emplace_or_replace<ParentComponent>(
            child,
            ParentAccess::make(new_parent)
        );
        return true;
    }

    inline void clearParent(
        lux::meta::EntityRegistryBase& registry,
        lux::meta::entity_id child)
    {
        if (child != lux::meta::null_entity && registry.valid(child))
            registry.remove<ParentComponent>(child);
    }

    namespace detail
    {
        inline std::size_t collectHierarchyCycleCores(
            lux::meta::EntityRegistry& registry,
            std::vector<lux::meta::entity_id>* cores)
        {
            auto& storage = registry.storage<ParentComponent>();
            std::size_t maximum_index = 0u;
            for (std::size_t index = 0u; index < storage.size(); ++index)
            {
                maximum_index = std::max<std::size_t>(
                    maximum_index,
                    entt::to_entity(storage.data()[index])
                );
            }
            std::vector<std::uint8_t> colour(maximum_index + 1u, 0u);
            std::vector<lux::meta::entity_id> stack;
            std::size_t count = 0u;

            for (std::size_t index = 0u; index < storage.size(); ++index)
            {
                auto entity = storage.data()[index];
                if (colour[entt::to_entity(entity)] != 0u)
                    continue;
                stack.clear();
                while (true)
                {
                    colour[entt::to_entity(entity)] = 1u;
                    stack.push_back(entity);
                    const auto parent = storage.contains(entity)
                        ? storage.get(entity).parent()
                        : lux::meta::null_entity;
                    if (!registry.valid(parent) || !storage.contains(parent))
                        break;
                    const auto parent_colour = colour[entt::to_entity(parent)];
                    if (parent_colour == 2u)
                        break;
                    if (parent_colour == 1u)
                    {
                        for (std::size_t cursor = stack.size(); cursor-- != 0u;)
                        {
                            ++count;
                            if (cores)
                                cores->push_back(stack[cursor]);
                            if (stack[cursor] == parent)
                                break;
                        }
                        break;
                    }
                    entity = parent;
                }
                for (const auto walked : stack)
                    colour[entt::to_entity(walked)] = 2u;
            }
            return count;
        }
    } // namespace detail

    [[nodiscard]] inline std::size_t validateHierarchy(
        lux::meta::EntityRegistry& registry)
    {
        return detail::collectHierarchyCycleCores(registry, nullptr);
    }

    inline std::size_t repairHierarchyCycles(lux::meta::EntityRegistry& registry)
    {
        std::vector<lux::meta::entity_id> cores;
        detail::collectHierarchyCycleCores(registry, &cores);
        for (const auto entity : cores)
            registry.remove<ParentComponent>(entity);
        return cores.size();
    }

    template <class Policy>
    class HierarchicalTransformSystem : public lux::ecs::ISystem
    {
    public:
        using Local = typename Policy::Local;
        using World = typename Policy::World;

        void update(const lux::ecs::SystemUpdateContext& context) override
        {
            auto& registry = context.registry();
            if (!derived_maintenance_connected_)
            {
                connectDerivedMaintenance<Local, World>(registry, scratch_);
                derived_maintenance_connected_ = true;
#ifndef NDEBUG
                maintenance_registry_ = &registry;
#endif
            }
#ifndef NDEBUG
            assert(maintenance_registry_ == &registry &&
                "Transform system reused across registries");
#endif
            if (!changes_.attached())
                attachChanges(registry);

            auto& hierarchy = registry.ctx().find<HierarchyIndex>()
                ? hierarchyIndex(registry)
                : ensureHierarchyIndex(registry);
            (void)hierarchy.refresh();
            const auto topology_revision = hierarchy.topologyRevision();
            const bool topology_changed =
                topology_revision != last_topology_revision_;
            last_topology_revision_ = topology_revision;
            const auto preorder = hierarchy.preorder();

            changed_.clear();
            for (const auto entity : changes_.view())
                changed_.push_back(entity);
            changes_.clear();

            intervals_.clear();
            standalone_.clear();
            if (topology_changed)
            {
                if (!preorder.empty())
                {
                    intervals_.push_back({
                        0u,
                        static_cast<std::uint32_t>(preorder.size())});
                }
                for (const auto entity : registry.view<Local, World>())
                {
                    if (!hierarchy.subtreeRange(entity))
                        standalone_.push_back(entity);
                }
            }
            else
            {
                for (const auto entity : changed_)
                {
                    if (const auto range = hierarchy.subtreeRange(entity))
                        intervals_.push_back(*range);
                    else
                        standalone_.push_back(entity);
                }
                mergeIntervals();
            }

            last_visited_count_ = 0u;
            for (const auto entity : standalone_)
                resolveOne(registry, entity);
            for (const auto range : intervals_)
            {
                const auto last = range.first + range.count;
                for (auto cursor = range.first; cursor < last; ++cursor)
                    resolveOne(registry, preorder[cursor]);
            }
        }

        void onRemoved(const SystemRemovalContext& removal) override
        {
            changes_.detach();
            if (derived_maintenance_connected_)
            {
                disconnectDerivedMaintenance<Local, World>(
                    removal.registry()
                );
                derived_maintenance_connected_ = false;
            }
            scratch_.clear();
            changed_.clear();
            standalone_.clear();
            intervals_.clear();
            last_visited_count_ = 0u;
            last_topology_revision_ = 0u;
#ifndef NDEBUG
            maintenance_registry_ = nullptr;
#endif
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

        [[nodiscard]] std::size_t lastVisitedCount() const noexcept
        {
            return last_visited_count_;
        }

    private:
        void attachChanges(lux::meta::EntityRegistry& registry)
        {
            changes_.attach(
                registry,
                static_cast<entt::id_type>(
                    systemType<HierarchicalTransformSystem<Policy>>().hash),
                [](auto& storage)
            {
                storage.template on_construct<Local>();
                storage.template on_update<Local>();
                storage.template on_construct<ParentComponent>();
                storage.template on_update<ParentComponent>();
                storage.template on_destroy<ParentComponent>();
            });
        }

        void mergeIntervals()
        {
            if (intervals_.size() < 2u)
                return;
            std::sort(
                intervals_.begin(),
                intervals_.end(),
                [](const auto& left, const auto& right)
                {
                    return left.first < right.first;
                }
            );
            std::size_t output = 0u;
            for (std::size_t index = 1u; index < intervals_.size(); ++index)
            {
                auto& current = intervals_[output];
                const auto current_last = current.first + current.count;
                const auto next_last =
                    intervals_[index].first + intervals_[index].count;
                if (intervals_[index].first <= current_last)
                {
                    current.count = std::max(current_last, next_last) - current.first;
                }
                else
                {
                    intervals_[++output] = intervals_[index];
                }
            }
            intervals_.resize(output + 1u);
        }

        void resolveOne(
            lux::meta::EntityRegistry& registry,
            lux::meta::entity_id entity)
        {
            auto* local = registry.try_get<Local>(entity);
            auto* world = registry.try_get<World>(entity);
            if (!local || !world)
                return;
            ++last_visited_count_;

            const World* parent_world = nullptr;
            if (const auto* parent = registry.try_get<ParentComponent>(entity);
                parent && registry.valid(parent->parent()))
            {
                parent_world = registry.try_get<World>(parent->parent());
            }

            const auto composed = parent_world
                ? Policy::composeChild(*local, *parent_world)
                : Policy::composeRoot(*local);
            if (world->position == composed.position &&
                world->linear == composed.linear)
            {
                return;
            }
            world->position = composed.position;
            world->linear = composed.linear;
            registry.patch<World>(entity);
        }

        ExtractionChangeSet<
            Local,
            ComponentList<World>,
            ComponentList<>> changes_;
        std::vector<lux::meta::entity_id> scratch_;
        std::vector<lux::meta::entity_id> changed_;
        std::vector<lux::meta::entity_id> standalone_;
        std::vector<HierarchySubtreeRange> intervals_;
        std::size_t last_visited_count_{0u};
        std::uint64_t last_topology_revision_{0u};
        bool derived_maintenance_connected_{false};
#ifndef NDEBUG
        const void* maintenance_registry_{nullptr};
#endif
    };
} // namespace lux::ecs
