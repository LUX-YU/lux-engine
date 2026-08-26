#include <lux/engine/simulation/ecs/hierarchy/detail/HierarchyMaintenance.hpp>

#include <algorithm>
#include <new>

namespace lux::simulation::ecs::detail
{
    HierarchyMaintenance::HierarchyMaintenance(
        Registry& registry,
        HierarchyIndex& hierarchy,
        HierarchyDeltaBatch& deltas
    )
        : registry_(std::addressof(registry)),
          hierarchy_(std::addressof(hierarchy)),
          deltas_(std::addressof(deltas)),
          constructed_(registry.on_construct<Parent>().connect<
              &HierarchyMaintenance::onParentConstruct>(*this)),
          updated_(registry.on_update<Parent>().connect<
              &HierarchyMaintenance::onParentUpdate>(*this)),
          destroyed_(registry.on_destroy<Parent>().connect<
              &HierarchyMaintenance::onParentDestroy>(*this))
    {
    }

    lux::cxx::expected<void, EHierarchyError> HierarchyMaintenance::prepare(
        std::size_t mutation_capacity
    ) noexcept
    {
        auto hierarchy_prepared = hierarchy_->prepare(mutation_capacity);
        if (!hierarchy_prepared)
            return hierarchy_prepared;
        try
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
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }

    void HierarchyMaintenance::onParentConstruct(
        Registry& registry,
        Entity entity
    ) noexcept
    {
        if (suppress_signals_)
            return;
        const auto* parent = registry.try_get<Parent>(entity);
        if (parent != nullptr)
        {
            (void)append(HierarchyMutation{
                EHierarchyMutationKind::SET_PARENT,
                entity,
                parent->entity});
        }
    }

    void HierarchyMaintenance::onParentUpdate(
        Registry& registry,
        Entity entity
    ) noexcept
    {
        onParentConstruct(registry, entity);
    }

    void HierarchyMaintenance::onParentDestroy(
        Registry&,
        Entity entity
    ) noexcept
    {
        if (suppress_signals_)
            return;
        (void)append(HierarchyMutation{
            EHierarchyMutationKind::REMOVE_PARENT,
            entity,
            NullEntity});
    }

    bool HierarchyMaintenance::append(HierarchyMutation mutation) noexcept
    {
        if (!exact_)
            return false;
        if (mutations_.size() >= capacity_)
        {
            exact_ = false;
            rebuild_required_ = true;
            return false;
        }
        mutations_.push_back(mutation);
        return true;
    }

    lux::cxx::expected<void, EHierarchyError>
    HierarchyMaintenance::rebuildFromRegistry() noexcept
    {
        mutations_.clear();
        invalid_entities_.clear();
        exact_ = true;
        for (auto [child, parent] : registry_->view<const Parent>().each())
        {
            if (parent.entity == NullEntity || child == parent.entity ||
                !registry_->valid(parent.entity))
            {
                if (invalid_entities_.size() >= capacity_)
                {
                    hierarchy_->invalidate(EHierarchyError::CAPACITY_EXCEEDED);
                    return lux::cxx::unexpected(
                        EHierarchyError::CAPACITY_EXCEEDED
                    );
                }
                invalid_entities_.push_back(child);
                continue;
            }
            if (!append(HierarchyMutation{
                    EHierarchyMutationKind::SET_PARENT,
                    child,
                    parent.entity}))
            {
                hierarchy_->invalidate(EHierarchyError::CAPACITY_EXCEEDED);
                return lux::cxx::unexpected(EHierarchyError::CAPACITY_EXCEEDED);
            }
        }

        suppress_signals_ = true;
        for (const Entity entity : invalid_entities_)
            registry_->remove<Parent>(entity);
        suppress_signals_ = false;

        auto rebuilt = hierarchy_->rebuild(mutations_, *deltas_);
        mutations_.clear();
        invalid_entities_.clear();
        rebuild_required_ = !rebuilt;
        return rebuilt;
    }

    lux::cxx::expected<void, EHierarchyError> HierarchyMaintenance::update()
        noexcept
    {
        deltas_->reset();
        if (!exact_ || rebuild_required_ || !hierarchy_->synchronized())
            return rebuildFromRegistry();

        bool invalid_parent{};
        for (auto [child, parent] : registry_->view<const Parent>().each())
        {
            if (parent.entity == NullEntity || child == parent.entity ||
                !registry_->valid(parent.entity))
            {
                invalid_parent = true;
                break;
            }
        }
        if (invalid_parent)
        {
            rebuild_required_ = true;
            return rebuildFromRegistry();
        }

        auto applied = hierarchy_->apply(mutations_, *deltas_);
        mutations_.clear();
        if (!applied)
            rebuild_required_ = true;
        return applied;
    }
}
