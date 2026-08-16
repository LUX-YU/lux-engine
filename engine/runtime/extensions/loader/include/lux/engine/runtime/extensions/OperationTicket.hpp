#pragma once
/**
 * @file OperationTicket.hpp
 * @brief Non-blocking, pollable status for domain activation operations.
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lux::extensions
{
    enum class EOperationTerminalState : std::uint8_t
    {
        PENDING,
        SUCCEEDED,
        FAILED,
        SUPERSEDED
    };

    template <class Phase, class Error, class Result>
    struct OperationSnapshot final
    {
        Phase phase{};
        EOperationTerminalState terminal{EOperationTerminalState::PENDING};
        std::optional<Error> error;
        std::optional<Result> result;
        std::uint64_t generation{0u};
    };

    namespace detail
    {
        template <class Phase, class Error, class Result>
        struct OperationTicketState final
        {
            explicit OperationTicketState(
                Phase initial_phase,
                std::uint64_t value_generation) noexcept
                : phase(initial_phase)
                , generation(value_generation)
            {}

            std::atomic<Phase> phase;
            std::atomic<EOperationTerminalState> terminal{
                EOperationTerminalState::PENDING};
            std::optional<Error> error;
            std::optional<Result> result;
            std::uint64_t generation{0u};
        };
    }

    template <class Phase, class Error, class Result>
    class OperationTicket final
    {
        static_assert(std::is_trivially_copyable_v<Phase>);
        static_assert(std::is_copy_constructible_v<Error>);
        static_assert(std::is_copy_constructible_v<Result>);

    public:
        OperationTicket() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(state_);
        }

        [[nodiscard]] OperationSnapshot<Phase, Error, Result>
        snapshot() const noexcept
        {
            if (!state_)
                return {};
            OperationSnapshot<Phase, Error, Result> value;
            value.phase = state_->phase.load(std::memory_order_relaxed);
            value.generation = state_->generation;
            value.terminal = state_->terminal.load(std::memory_order_acquire);
            if (value.terminal == EOperationTerminalState::FAILED)
                value.error = state_->error;
            else if (value.terminal == EOperationTerminalState::SUCCEEDED)
                value.result = state_->result;
            return value;
        }

    private:
        template <class P, class E, class R>
        friend class OperationTicketPublisher;

        explicit OperationTicket(
            std::shared_ptr<detail::OperationTicketState<Phase, Error, Result>>
                state) noexcept
            : state_(std::move(state))
        {}

        std::shared_ptr<detail::OperationTicketState<Phase, Error, Result>>
            state_;
    };

    template <class Phase, class Error, class Result>
    class OperationTicketPublisher final
    {
    public:
        OperationTicketPublisher(
            Phase initial_phase,
            std::uint64_t generation)
            : state_(std::make_shared<
                  detail::OperationTicketState<Phase, Error, Result>>(
                  initial_phase,
                  generation))
        {}

        [[nodiscard]] OperationTicket<Phase, Error, Result> ticket() const
            noexcept
        {
            return OperationTicket<Phase, Error, Result>{state_};
        }

        void setPhase(Phase phase) noexcept
        {
            if (pending())
                state_->phase.store(phase, std::memory_order_relaxed);
        }

        void succeed(Result result) noexcept
        {
            if (!pending())
                return;
            state_->result.emplace(std::move(result));
            state_->terminal.store(
                EOperationTerminalState::SUCCEEDED,
                std::memory_order_release);
        }

        void fail(Error error) noexcept
        {
            if (!pending())
                return;
            state_->error.emplace(std::move(error));
            state_->terminal.store(
                EOperationTerminalState::FAILED,
                std::memory_order_release);
        }

        void supersede() noexcept
        {
            EOperationTerminalState expected = EOperationTerminalState::PENDING;
            (void)state_->terminal.compare_exchange_strong(
                expected,
                EOperationTerminalState::SUPERSEDED,
                std::memory_order_release,
                std::memory_order_relaxed);
        }

    private:
        [[nodiscard]] bool pending() const noexcept
        {
            return state_->terminal.load(std::memory_order_relaxed) ==
                EOperationTerminalState::PENDING;
        }

        std::shared_ptr<detail::OperationTicketState<Phase, Error, Result>>
            state_;
    };
}
