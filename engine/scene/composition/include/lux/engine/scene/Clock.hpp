#pragma once

#include <lux/engine/simulation/SimulationTime.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <chrono>
#include <concepts>
#include <limits>
#include <optional>
#include <type_traits>
#include <variant>

namespace lux::scene
{
    enum class EClockError : std::uint8_t
    {
        INVALID_STEP,
        TIME_OVERFLOW,
    };

    using ClockSample = lux::cxx::expected<std::optional<simulation::SimulationDuration>, EClockError>;

    template <class T>
    concept Clock = std::movable<T> && std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T> &&
                    requires(
                        T& clock,
                        const T& source,
                        std::chrono::steady_clock::time_point now,
                        const simulation::SimulationTime& adopted
                    ) {
                        { source.sample(now) } noexcept -> std::same_as<ClockSample>;
                        { clock.adopt(now, adopted) } noexcept -> std::same_as<void>;
                        { clock.rebase(now) } noexcept -> std::same_as<void>;
                        { source.snapshot() } noexcept -> std::same_as<simulation::SimulationTime>;
                        {
                            source.deadline()
                        } noexcept -> std::same_as<std::optional<std::chrono::steady_clock::time_point>>;
                    };

    class FixedStepClock final
    {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;
        using CreateResult = lux::cxx::expected<FixedStepClock, EClockError>;

        FixedStepClock() noexcept = default;

        [[nodiscard]] static CreateResult create(
            simulation::SimulationDuration step = std::chrono::milliseconds{16}
        ) noexcept
        {
            if (step.count() <= 0)
            {
                return lux::cxx::unexpected<EClockError>(EClockError::INVALID_STEP);
            }
            FixedStepClock result;
            result.step_ = step;
            return result;
        }

        [[nodiscard]] ClockSample sample(TimePoint now) const noexcept
        {
            if (deadline_ && now < *deadline_)
            {
                return std::optional<simulation::SimulationDuration>{};
            }
            const bool is_step_overflow = time_.step_index == std::numeric_limits<std::uint64_t>::max();
            const bool is_time_overflow = time_.elapsed > simulation::SimulationDuration::max() - step_;
            const bool is_deadline_overflow = now > TimePoint::max() - step_;
            const bool is_overflow = is_step_overflow || is_time_overflow || is_deadline_overflow;
            if (is_overflow)
            {
                return lux::cxx::unexpected<EClockError>(EClockError::TIME_OVERFLOW);
            }
            return std::optional{time_.elapsed + step_};
        }

        // The runtime supplies the actual execution result, including a partially failed step.
        void adopt(TimePoint now, const simulation::SimulationTime& time) noexcept
        {
            time_ = time;
            deadline_ = now > TimePoint::max() - step_ ? TimePoint::max() : now + step_;
        }

        void rebase(TimePoint now) noexcept
        {
            deadline_ = now;
        }
        [[nodiscard]] simulation::SimulationTime snapshot() const noexcept
        {
            return time_;
        }
        [[nodiscard]] std::optional<TimePoint> deadline() const noexcept
        {
            return deadline_;
        }

    private:
        simulation::SimulationDuration step_{std::chrono::milliseconds{16}};
        simulation::SimulationTime time_;
        std::optional<TimePoint> deadline_;
    };

    static_assert(Clock<FixedStepClock>);
    using VSimulationClock = std::variant<FixedStepClock>;
}
