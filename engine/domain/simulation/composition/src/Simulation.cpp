#include <lux/engine/simulation/Simulation.hpp>

#include <lux/engine/simulation/SimulationSystem.hpp>
#include <utility>

namespace lux::simulation
{
    Simulation::Simulation() noexcept = default;

    Simulation::~Simulation() noexcept
    {
        // Later groups may borrow earlier groups' systems.
        while (!groups_.empty())
        {
            groups_.pop_back();
        }
    }

    void Simulation::add(TickGroup group) noexcept
    {
        groups_.push_back(std::move(group));
    }

    std::span<const TickGroup> Simulation::groups() const noexcept
    {
        return groups_;
    }

    void
    Simulation::bindExecution(
        execution::TaskScope& tasks,
        ecs::EcsCommandBuffer& commands,
        std::span<const std::size_t> producers
    ) noexcept
    {
        std::size_t index = 0;
        for (auto& group : groups_)
        {
            for (auto& system : group.systems_)
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
    }

    exec::task<Simulation::Result> Simulation::releaseResources() noexcept
    {
        while (!groups_.empty())
        {
            auto& systems = groups_.back().systems_;
            while (!systems.empty())
            {
                systems.back()->beginDestruction();
                auto released = co_await systems.back()->releaseResources();
                if (!released)
                {
                    co_return released;
                }
                systems.pop_back();
            }
            groups_.pop_back();
        }
        co_return Result{};
    }
} // namespace lux::simulation
