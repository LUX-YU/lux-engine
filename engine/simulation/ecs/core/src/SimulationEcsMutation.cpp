#include <lux/engine/simulation/ecs/SimulationEcsMutation.hpp>

#include <lux/engine/simulation/ecs/core/detail/EcsChangeJournalAccess.hpp>

#include <entt/core/type_info.hpp>

#include <utility>

namespace lux::simulation::ecs
{
    SimulationEcsMutation::SimulationEcsMutation() noexcept = default;
    SimulationEcsMutation::SimulationEcsMutation(
        SimulationEcsMutation&&
    ) noexcept = default;
    SimulationEcsMutation& SimulationEcsMutation::operator=(
        SimulationEcsMutation&&
    ) noexcept = default;

    SimulationEcsMutation::SimulationEcsMutation(
        EcsState& state,
        EcsMutation&& mutation,
        EcsChangeJournal& journal
    ) noexcept
        : state_(&state),
          mutation_(std::move(mutation)),
          publisher_log_(&detail::EcsChangeJournalAccess::log(journal))
    {
    }

    SimulationEcsMutation::operator bool() const noexcept
    {
        return state_ != nullptr && static_cast<bool>(mutation_);
    }

    const EcsState& SimulationEcsMutation::state() const noexcept
    {
        detail::require(state_ != nullptr && mutation_);
        return *state_;
    }

    EcsMutation& SimulationEcsMutation::mutation() noexcept
    {
        detail::require(state_ != nullptr && mutation_);
        return mutation_;
    }

    Entity SimulationEcsMutation::create()
    {
        const Entity entity = mutation().create();
        if (publisher_exact_)
        {
            auto& log = *static_cast<detail::EcsChangeLog*>(publisher_log_);
            publisher_exact_ = log.recordEntity(
                entity,
                EEntityChangeKind::ADDED
            );
        }
        return entity;
    }

    void SimulationEcsMutation::destroy(Entity entity)
    {
        detail::require(state_ != nullptr && state_->valid(entity));
        const auto entity_storage = entt::type_hash<Entity>::value();
        auto& log = *static_cast<detail::EcsChangeLog*>(publisher_log_);
        for (auto&& [storage_id, storage] : state_->registry_.storage())
        {
            if (storage_id == entity_storage || !storage.contains(entity))
                continue;
            if (!publisher_exact_)
                continue;
            auto stream = log.bindComponent(storage_id);
            publisher_exact_ = stream && stream(
                entity,
                EComponentChangeKind::REMOVED
            );
        }
        mutation().destroy(entity);
        if (publisher_exact_)
        {
            publisher_exact_ = log.recordEntity(
                entity,
                EEntityChangeKind::DESTROYED
            );
        }
    }

    void SimulationEcsMutation::recordComponent(
        std::uint64_t storage,
        Entity entity,
        EComponentChangeKind kind
    ) noexcept
    {
        if (!publisher_exact_)
            return;
        auto& log = *static_cast<detail::EcsChangeLog*>(publisher_log_);
        auto stream = log.bindComponent(storage);
        publisher_exact_ = stream && stream(entity, kind);
    }

    lux::cxx::expected<SimulationEcsMutation, EcsMutationError>
    beginSimulationEcsMutation(
        EcsState& state,
        EcsChangeJournal& journal
    ) noexcept
    {
        auto canonical = state.mutate();
        if (!canonical)
            return lux::cxx::unexpected(canonical.error());
        return SimulationEcsMutation(
            state,
            std::move(*canonical),
            journal
        );
    }
} // namespace lux::simulation::ecs
