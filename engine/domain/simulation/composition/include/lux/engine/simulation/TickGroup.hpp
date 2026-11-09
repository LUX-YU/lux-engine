#pragma once

#include <lux/engine/simulation/SimulationTime.hpp>
#include <lux/engine/simulation/composition/visibility.h>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lux::simulation
{
    class SimulationSystem;

    // Owns systems that advance together. Engine compiles their accesses into one task graph.
    class LUX_ENGINE_SIMULATION_COMPOSITION_PUBLIC TickGroup final
    {
    public:
        using Duration = SimulationDuration;

        // No fixed step: execute once per admitted Engine update.
        TickGroup() noexcept;
        explicit TickGroup(Duration fixed_step) noexcept;
        ~TickGroup() noexcept;

        TickGroup(TickGroup&&) noexcept;
        TickGroup& operator=(TickGroup&&) noexcept;
        TickGroup(const TickGroup&) = delete;
        TickGroup& operator=(const TickGroup&) = delete;

        // Assembly only, before Engine adoption. Takes a non-null system.
        void add(std::unique_ptr<SimulationSystem> system) noexcept;

        [[nodiscard]] std::optional<Duration> fixedStep() const noexcept;
        [[nodiscard]] std::span<const std::unique_ptr<SimulationSystem>> systems() const noexcept;

    private:
        friend class Simulation;
        std::optional<Duration> fixed_step_;
        std::vector<std::unique_ptr<SimulationSystem>> systems_;
    };
} // namespace lux::simulation
