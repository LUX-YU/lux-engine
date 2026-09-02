#pragma once

#include <chrono>
#include <cstdint>

namespace lux::simulation
{
    using SimulationDuration = std::chrono::nanoseconds;

    struct SimulationClockSnapshot final
    {
        SimulationDuration elapsed{};
        SimulationDuration delta{};
        std::uint64_t step_index{};
    };

    class SimulationClock final
    {
    public:
        SimulationClock() noexcept = default;

        [[nodiscard]] SimulationClockSnapshot snapshot() const noexcept
        {
            return {elapsed_, delta_, step_index_};
        }

    private:
        void advance(SimulationDuration delta) noexcept
        {
            elapsed_ += delta;
            delta_ = delta;
            ++step_index_;
        }

        SimulationDuration elapsed_{};
        SimulationDuration delta_{};
        std::uint64_t step_index_{};

        friend class Simulation;
    };
} // namespace lux::simulation
