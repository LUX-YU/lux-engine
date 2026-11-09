#include <lux/engine/simulation/SimulationSystem.hpp>

namespace lux::simulation
{
    ecs::EcsCommandProducerCapacity SimulationSystem::commandCapacity() const noexcept
    {
        return {0, 0};
    }

    cxx::expected<ecs::EcsCommandWriter, ecs::EcsCommandFailure>
    SimulationSystem::beginCommands(ecs::EEcsCommandPolicy policy) noexcept
    {
        if (!commands_ || command_producer_ == SIZE_MAX)
        {
            return cxx::unexpected(ecs::EcsCommandFailure{ecs::EEcsCommandError::INVALID_PRODUCER});
        }
        return commands_->begin(command_producer_, policy);
    }

    std::optional<ecs::Entity> SimulationSystem::resolveEntity(ecs::DeferredEntity entity) const noexcept
    {
        if (!commands_ || entity.producer != command_producer_)
        {
            return std::nullopt;
        }
        return commands_->resolve(entity);
    }

    task::ETaskAffinity SimulationSystem::taskAffinity() const noexcept
    {
        return task::ETaskAffinity::CALLER_THREAD;
    }

    void SimulationSystem::setEnable(bool enabled) noexcept
    {
        enabled_ = enabled;
    }

    bool SimulationSystem::isEnabled() const noexcept
    {
        return enabled_;
    }

    execution::TaskScope& SimulationSystem::taskScope() const noexcept
    {
        if (!tasks_)
        {
            std::terminate(); // Task submission requires Engine adoption.
        }
        return *tasks_;
    }

    exec::task<SimulationSystem::Result> SimulationSystem::releaseResources() noexcept
    {
        co_return Result{};
    }

} // namespace lux::simulation
