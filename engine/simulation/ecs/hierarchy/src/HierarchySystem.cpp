#include <lux/engine/simulation/ecs/HierarchySystem.hpp>

#include <lux/engine/simulation/ecs/core/detail/EcsStateAccess.hpp>

namespace lux::simulation::ecs
{
    namespace
    {
        struct EraseOrphanedParent final
        {
            Entity child{NullEntity};
            Entity destroyed_parent{NullEntity};

            void apply(SimulationEcsMutation& mutation) noexcept
            {
                const EcsState& state = mutation.state();
                const Parent* current = state.find<Parent>(child);
                if (state.valid(child) && current != nullptr &&
                    current->entity == destroyed_parent)
                {
                    mutation.erase<Parent>(child);
                }
            }
        };
    } // namespace

    HierarchySystem::HierarchySystem(
        EcsState& state,
        HierarchyIndex& hierarchy,
        HierarchyMutationBatch& mutations,
        HierarchyDeltaBatch& deltas
    ) noexcept
        : state_(std::addressof(state)),
          hierarchy_(std::addressof(hierarchy)),
          mutations_(std::addressof(mutations)),
          deltas_(std::addressof(deltas))
    {
    }

    void HierarchySystem::update(
        EcsState& state,
        EcsChangeJournal& journal,
        EcsCommands commands
    ) noexcept
    {
        detail::require(
            state_ == std::addressof(state) && hierarchy_ != nullptr &&
            mutations_ != nullptr && deltas_ != nullptr
        );
        mutations_->reset();
        deltas_->reset();

        auto parent_changes = componentChanges(journal, parent_cursor_);
        auto entity_changes = lux::simulation::ecs::entityChanges(
            journal,
            entity_cursor_
        );
        bool rebuild = rebuild_required_ || !hierarchy_->synchronized() ||
            parent_changes.status() == EChangeReadStatus::RESYNC_REQUIRED ||
            entity_changes.status() == EChangeReadStatus::RESYNC_REQUIRED;
        bool pending_repair{};

        const auto queue_repair = [&](Entity child, Entity parent) noexcept
        {
            const ECommandResult result = commands.push(
                EraseOrphanedParent{child, parent}
            );
            if (result != ECommandResult::ACCEPTED)
            {
                hierarchy_->invalidate(EHierarchyError::CAPACITY_EXCEEDED);
                rebuild_required_ = true;
                return false;
            }
            pending_repair = true;
            rebuild_required_ = true;
            return true;
        };

        const auto append_parent = [&](Entity child, const Parent& link) noexcept
        {
            if (link.entity == NullEntity || child == link.entity)
            {
                hierarchy_->invalidate(
                    child == link.entity
                        ? EHierarchyError::SELF_PARENT
                        : EHierarchyError::INVALID_PARENT
                );
                return false;
            }
            const auto reference = detail::EcsEntityAccess::referenceState(
                state,
                link.entity
            );
            if (reference == detail::EEntityReferenceState::UNKNOWN)
            {
                hierarchy_->invalidate(EHierarchyError::INVALID_PARENT);
                return false;
            }
            if (reference == detail::EEntityReferenceState::STALE)
                return queue_repair(child, link.entity);
            return mutations_->append(HierarchyMutation{
                EHierarchyMutationKind::SET_PARENT,
                child,
                link.entity
            });
        };

        const auto rebuild_from_canonical = [&]() noexcept
        {
            mutations_->reset();
            pending_repair = false;
            for (auto [child, parent] : state.query<Read<Parent>>())
            {
                if (!append_parent(child, parent))
                {
                    if (!mutations_->exact())
                    {
                        hierarchy_->invalidate(
                            EHierarchyError::CAPACITY_EXCEEDED
                        );
                    }
                    return false;
                }
            }
            if (!mutations_->exact())
            {
                hierarchy_->invalidate(EHierarchyError::CAPACITY_EXCEEDED);
                return false;
            }
            auto rebuilt = hierarchy_->rebuild(mutations_->values(), *deltas_);
            if (!rebuilt)
                return false;
            if (pending_repair)
            {
                hierarchy_->invalidate(EHierarchyError::NOT_SYNCHRONIZED);
                rebuild_required_ = true;
            }
            else
                rebuild_required_ = false;
            return true;
        };

        if (rebuild)
        {
            (void)rebuild_from_canonical();
            return;
        }

        for (const EntityChange change : entity_changes)
        {
            if (change.kind != EEntityChangeKind::DESTROYED)
                continue;
            for (const Entity child : hierarchy_->children(change.entity))
            {
                if (!queue_repair(child, change.entity))
                    return;
            }
            if (!mutations_->append(HierarchyMutation{
                    EHierarchyMutationKind::ENTITY_DESTROYED,
                    change.entity,
                    NullEntity
                }))
            {
                rebuild = true;
                break;
            }
        }

        if (!rebuild)
        {
            for (const ComponentChange change : parent_changes)
            {
                if (!state.valid(change.entity))
                    continue;
                const Parent* parent = state.find<Parent>(change.entity);
                if (parent == nullptr)
                {
                    if (!mutations_->append(HierarchyMutation{
                            EHierarchyMutationKind::REMOVE_PARENT,
                            change.entity,
                            NullEntity
                        }))
                    {
                        rebuild = true;
                        break;
                    }
                }
                else if (!append_parent(change.entity, *parent))
                {
                    if (!mutations_->exact())
                        rebuild = true;
                    else if (!hierarchy_->synchronized())
                        return;
                }
            }
        }

        if (rebuild || !mutations_->exact())
        {
            rebuild_required_ = true;
            (void)rebuild_from_canonical();
            return;
        }

        auto applied = hierarchy_->apply(mutations_->values(), *deltas_);
        if (!applied)
        {
            rebuild_required_ = true;
            return;
        }
        if (pending_repair)
        {
            hierarchy_->invalidate(EHierarchyError::NOT_SYNCHRONIZED);
            rebuild_required_ = true;
        }
    }
} // namespace lux::simulation::ecs
