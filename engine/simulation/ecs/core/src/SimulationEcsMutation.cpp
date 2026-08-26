#include <lux/engine/simulation/ecs/SimulationEcsMutation.hpp>

#include <lux/engine/simulation/ecs/core/detail/EcsChangeJournalAccess.hpp>

#include <entt/core/type_info.hpp>

#include <utility>

namespace lux::simulation::ecs
{
    struct SimulationEcsMutation::Impl final
    {
        Impl(
            EcsState& value,
            EcsMutation&& canonical,
            EcsChangeJournal& journal
        ) noexcept
            : state(&value), mutation(std::move(canonical)), publisher(journal)
        {
        }

        EcsState* state{};
        EcsMutation mutation;
        detail::EcsChangePublisher publisher;
    };

    SimulationEcsMutation::SimulationEcsMutation() noexcept = default;
    SimulationEcsMutation::~SimulationEcsMutation() noexcept = default;
    SimulationEcsMutation::SimulationEcsMutation(
        SimulationEcsMutation&&
    ) noexcept = default;
    SimulationEcsMutation& SimulationEcsMutation::operator=(
        SimulationEcsMutation&&
    ) noexcept = default;

    SimulationEcsMutation::SimulationEcsMutation(
        std::unique_ptr<Impl> impl
    ) noexcept
        : impl_(std::move(impl))
    {
    }

    SimulationEcsMutation::operator bool() const noexcept
    {
        return impl_ != nullptr && static_cast<bool>(impl_->mutation);
    }

    const EcsState& SimulationEcsMutation::state() const noexcept
    {
        detail::require(impl_ != nullptr && impl_->state != nullptr);
        return *impl_->state;
    }

    EcsMutation& SimulationEcsMutation::mutation() noexcept
    {
        detail::require(impl_ != nullptr && impl_->mutation);
        return impl_->mutation;
    }

    Entity SimulationEcsMutation::create()
    {
        const Entity entity = mutation().create();
        (void)impl_->publisher.appendEntity(
            entity,
            EEntityChangeKind::ADDED
        );
        return entity;
    }

    void SimulationEcsMutation::destroy(Entity entity)
    {
        detail::require(impl_ != nullptr && impl_->state->valid(entity));
        const auto entity_storage = entt::type_hash<Entity>::value();
        for (auto&& [storage_id, storage] : impl_->state->registry_.storage())
        {
            if (storage_id == entity_storage || !storage.contains(entity))
                continue;
            auto stream = impl_->publisher.bindComponent(storage_id);
            (void)impl_->publisher.append(
                stream,
                entity,
                EComponentChangeKind::REMOVED
            );
        }
        mutation().destroy(entity);
        (void)impl_->publisher.appendEntity(
            entity,
            EEntityChangeKind::DESTROYED
        );
    }

    void SimulationEcsMutation::recordComponent(
        std::uint64_t storage,
        Entity entity,
        EComponentChangeKind kind
    ) noexcept
    {
        auto stream = impl_->publisher.bindComponent(storage);
        (void)impl_->publisher.append(stream, entity, kind);
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
        try
        {
            return SimulationEcsMutation(std::make_unique<
                SimulationEcsMutation::Impl>(
                    state,
                    std::move(*canonical),
                    journal
                ));
        }
        catch (...)
        {
            return lux::cxx::unexpected(EcsMutationError{
                EEcsMutationError::ALLOCATION_FAILURE
            });
        }
    }
} // namespace lux::simulation::ecs
