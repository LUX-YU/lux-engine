#include <lux/engine/simulation/ecs/hierarchy/detail/HierarchyMaintenance.hpp>

#include <algorithm>

namespace lux::simulation::ecs::detail
{
    HierarchyMaintenance::HierarchyMaintenance(
        Registry& registry,
        HierarchyIndex& hierarchy,
        HierarchyDeltaBatch& deltas
    )
        : registry_(std::addressof(registry)), hierarchy_(std::addressof(hierarchy)), deltas_(std::addressof(deltas)),
          constructed_(registry.on_construct<Parent>().connect<&HierarchyMaintenance::onParentConstruct>(*this)),
          updated_(registry.on_update<Parent>().connect<&HierarchyMaintenance::onParentUpdate>(*this)),
          destroyed_(registry.on_destroy<Parent>().connect<&HierarchyMaintenance::onParentDestroy>(*this)),
          entity_destroyed_(registry.on_destroy<Entity>().connect<&HierarchyMaintenance::onEntityDestroy>(*this))
    {}

    lux::cxx::expected<void, EHierarchyError> HierarchyMaintenance::prepare(std::size_t mutation_capacity) noexcept
    {
        auto hierarchy_prepared = hierarchy_->prepare(mutation_capacity);
        if (!hierarchy_prepared)
        {
            return hierarchy_prepared;
        }
        {
            mutations_.clear();
            invalid_entities_.clear();
            mutations_.reserve(mutation_capacity);
            invalid_entities_.reserve(mutation_capacity);
            capacity_ = mutation_capacity;
            exact_ = true;
            rebuild_required_ = true;
            return {};
        }
    }

    void HierarchyMaintenance::onParentConstruct(Registry& registry, Entity entity) noexcept
    {
        const auto* parent = registry.try_get<Parent>(entity);
        if (parent != nullptr)
        {
            const auto kind = parent->entity == NullEntity ? EHierarchyMutationKind::REMOVE_PARENT
                                                           : EHierarchyMutationKind::SET_PARENT;
            (void)append(HierarchyMutation{kind, entity, parent->entity});
        }
    }

    void HierarchyMaintenance::onParentUpdate(Registry& registry, Entity entity) noexcept
    {
        onParentConstruct(registry, entity);
    }

    void HierarchyMaintenance::onParentDestroy(Registry&, Entity entity) noexcept
    {
        (void)append(HierarchyMutation{EHierarchyMutationKind::REMOVE_PARENT, entity, NullEntity});
    }

    void HierarchyMaintenance::onEntityDestroy(Registry&, Entity entity) noexcept
    {
        // A root may have children without a Parent component of its own. Its
        // full identity must retire before a later mutation reuses the slot.
        (void)append(HierarchyMutation{EHierarchyMutationKind::ENTITY_DESTROYED, entity, NullEntity});
    }

    bool HierarchyMaintenance::append(HierarchyMutation mutation) noexcept
    {
        if (!exact_)
        {
            return false;
        }
        if (mutations_.size() >= capacity_)
        {
            exact_ = false;
            rebuild_required_ = true;
            return false;
        }
        mutations_.push_back(mutation);
        return true;
    }

    lux::cxx::expected<void, EHierarchyError> HierarchyMaintenance::rebuildFromRegistry(EcsCommandWriter& commands
    ) noexcept
    {
        mutations_.clear();
        invalid_entities_.clear();
        exact_ = true;
        for (auto [child, parent] : registry_->view<const Parent>().each())
        {
            if (parent.entity == NullEntity || child == parent.entity || !registry_->valid(parent.entity))
            {
                if (invalid_entities_.size() >= capacity_)
                {
                    hierarchy_->invalidate(EHierarchyError::CAPACITY_EXCEEDED);
                    return lux::cxx::unexpected(EHierarchyError::CAPACITY_EXCEEDED);
                }
                invalid_entities_.push_back(child);
                continue;
            }
            if (!append(HierarchyMutation{EHierarchyMutationKind::SET_PARENT, child, parent.entity}))
            {
                hierarchy_->invalidate(EHierarchyError::CAPACITY_EXCEEDED);
                return lux::cxx::unexpected(EHierarchyError::CAPACITY_EXCEEDED);
            }
        }

        for (const Entity entity : invalid_entities_)
        {
            if (!commands.remove<Parent>(entity))
            {
                return lux::cxx::unexpected(EHierarchyError::COMMAND_RECORDING_FAILED);
            }
        }

        auto rebuilt = hierarchy_->rebuild(mutations_, *deltas_);
        mutations_.clear();
        invalid_entities_.clear();
        rebuild_required_ = !rebuilt;
        return rebuilt;
    }

    bool HierarchyMaintenance::hasPendingChanges() const noexcept
    {
        return !mutations_.empty() || !exact_ || rebuild_required_ || !hierarchy_->synchronized();
    }

    lux::cxx::expected<void, EHierarchyError> HierarchyMaintenance::update(EcsCommandWriter& commands) noexcept
    {
        deltas_->reset();
        if (!hasPendingChanges())
            return {};
        if (!exact_ || rebuild_required_ || !hierarchy_->synchronized())
        {
            return rebuildFromRegistry(commands);
        }

        bool invalid_parent{};
        for (auto [child, parent] : registry_->view<const Parent>().each())
        {
            if (parent.entity == NullEntity || child == parent.entity || !registry_->valid(parent.entity))
            {
                invalid_parent = true;
                break;
            }
        }
        if (invalid_parent)
        {
            rebuild_required_ = true;
            return rebuildFromRegistry(commands);
        }

        auto applied = hierarchy_->apply(mutations_, *deltas_);
        mutations_.clear();
        if (!applied)
        {
            rebuild_required_ = true;
        }
        return applied;
    }
} // namespace lux::simulation::ecs::detail
