#include <lux/engine/scene/SceneInstance.hpp>

#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/world/World.hpp>

#include <utility>

namespace lux::scene
{
    SceneInstance::SceneInstance(
        std::unique_ptr<world::World> world,
        std::unique_ptr<simulation::Simulation> simulation
    ) noexcept
        : world_(std::move(world)), simulation_(std::move(simulation))
    {
    }

    SceneInstance::~SceneInstance() noexcept
    {
        // Reverse insertion order is explicit; vector element destruction order is not a dependency contract.
        while (!systems_.empty())
        {
            systems_.pop_back();
        }
    }

    world::World& SceneInstance::world() noexcept
    {
        return *world_;
    }

    simulation::Simulation& SceneInstance::simulation() noexcept
    {
        return *simulation_;
    }

    std::span<const std::unique_ptr<SceneSystem>> SceneInstance::systems() const noexcept
    {
        return systems_;
    }

    void SceneInstance::add(std::unique_ptr<SceneSystem> system) noexcept
    {
        systems_.push_back(std::move(system));
    }

    void
    SceneInstance::bindExecution(
        execution::TaskScope& tasks,
        ecs::EcsCommandBuffer& commands,
        std::span<const std::size_t> producers
    ) noexcept
    {
        const auto simulation_count = producers.size() - systems_.size();
        simulation_->bindExecution(tasks, commands, producers.first(simulation_count));
        std::size_t index = simulation_count;
        for (auto& system : systems_)
        {
            if (system->tasks_)
            {
                std::terminate();
            }
            system->tasks_ = &tasks;
            system->commands_ = &commands;
            system->command_producer_ = producers[index++];
        }
    }

    SceneInstance::Result SceneInstance::applyCommands(ecs::EcsCommandBuffer& commands) noexcept
    {
        auto applied = ecs::applyEcsCommands(world_->registry(), commands);
        if (!applied)
        {
            const auto& failure = applied.error();
            return cxx::unexpected(error::Error{
                error::errorId("lux.scene.ecs_commands"),
                {static_cast<std::uint64_t>(failure.code), failure.producer, failure.command}
            });
        }
        return {};
    }

    exec::task<SceneInstance::Result> SceneInstance::initialize(ecs::EcsCommandBuffer& commands) noexcept
    {
        for (auto& system : systems_)
        {
            if (system->taskScope().stopToken().stop_requested())
            {
                co_return cxx::unexpected(execution::toError(execution::EExecutionError::STOPPING));
            }
            auto initialized = co_await system->initialize();
            if (!initialized)
            {
                co_return initialized;
            }
            if (auto applied = applyCommands(commands); !applied)
            {
                co_return applied;
            }
        }
        co_return Result{};
    }

    exec::task<SceneInstance::Result> SceneInstance::releaseResources() noexcept
    {
        while (!systems_.empty())
        {
            systems_.back()->beginDestruction(); // Revoke queued Object callbacks before releasing derived resources.
            auto released = co_await systems_.back()->releaseResources();
            if (!released)
            {
                co_return released;
            }
            systems_.pop_back();
        }
        co_return co_await simulation_->releaseResources();
    }
} // namespace lux::scene
