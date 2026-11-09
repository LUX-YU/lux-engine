#pragma once

#include <exec/task.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/ecs/EcsCommandBuffer.hpp>

#include <lux/engine/simulation/TickGroup.hpp>

namespace lux::scene
{
    class SceneInstance;
}

namespace lux::simulation
{
    // Owns externally assembled groups. Group and system additions end at Engine adoption.
    class LUX_ENGINE_SIMULATION_COMPOSITION_PUBLIC Simulation final
    {
    public:
        Simulation() noexcept;
        ~Simulation() noexcept;
        Simulation(const Simulation&) = delete;
        Simulation& operator=(const Simulation&) = delete;

        void add(TickGroup group) noexcept;
        [[nodiscard]] std::span<const TickGroup> groups() const noexcept;

    private:
        friend class scene::SceneInstance;
        using Result = cxx::expected<void, error::Error>;
        void
        bindExecution(
            execution::TaskScope& tasks,
            ecs::EcsCommandBuffer& commands,
            std::span<const std::size_t> producers
        ) noexcept;
        [[nodiscard]] exec::task<Result> releaseResources() noexcept;

        std::vector<TickGroup> groups_;
    };
} // namespace lux::simulation
