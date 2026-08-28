#pragma once
/**
 * @file PortSender.hpp
 * @brief Allocation-free OperationPort to stdexec Sender adapter.
 */

#include <lux/engine/core/async/OperationPort.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <utility>

namespace lux::process
{
    template <lux::async::Operation Operation>
    class PortSender final
    {
    public:
        using Value = typename Operation::Value;
        using Failure = lux::async::OperationFailure<typename Operation::Error>;
        using Outcome = lux::async::OperationOutcome<Operation>;
        using sender_concept = stdexec::sender_t;
        using completion_signatures = std::conditional_t<
            std::is_void_v<Value>,
            stdexec::completion_signatures<
                stdexec::set_value_t(),
                stdexec::set_error_t(Failure),
                stdexec::set_stopped_t()
            >,
            stdexec::completion_signatures<
                stdexec::set_value_t(Value),
                stdexec::set_error_t(Failure),
                stdexec::set_stopped_t()
            >
        >;

        PortSender(
            lux::async::OperationPort<Operation> port,
            Operation operation,
            lux::async::SubmitOptions options
        ) noexcept
            : port_(std::move(port)), operation_(std::move(operation)), options_(options)
        {
        }

        template <class Receiver>
        class State final
        {
        public:
            using operation_state_concept = stdexec::operation_state_t;

            State(
                lux::async::OperationPort<Operation> port,
                Operation operation,
                lux::async::SubmitOptions options,
                Receiver receiver
            )
                : port_(std::move(port)),
                  operation_(std::move(operation)),
                  options_(options),
                  receiver_(std::move(receiver))
            {
            }

            State(const State&) = delete;
            State& operator=(const State&) = delete;
            State(State&&) = delete;
            State& operator=(State&&) = delete;

            void start() & noexcept
            {
                const auto token = stdexec::get_stop_token(stdexec::get_env(receiver_));
                if (token.stop_requested())
                {
                    phase_.store(EPhase::COMPLETED, std::memory_order_release);
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }

                const auto submitted = port_.submit(std::move(operation_), this, &State::complete, options_);
                if (submitted)
                {
                    auto expected = EPhase::SUBMITTING;
                    if (!phase_.compare_exchange_strong(
                            expected,
                            EPhase::ACCEPTED,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire
                        ) && expected != EPhase::COMPLETED)
                    {
                        std::terminate();
                    }
                    return;
                }

                auto expected = EPhase::SUBMITTING;
                if (phase_.compare_exchange_strong(
                        expected,
                        EPhase::COMPLETED,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire
                    ))
                {
                    stdexec::set_error(
                        std::move(receiver_),
                        Failure::runtime(submitted.error())
                    );
                }
                else if (expected != EPhase::COMPLETED)
                {
                    std::terminate();
                }
            }

        private:
            enum class EPhase : std::uint8_t
            {
                SUBMITTING,
                ACCEPTED,
                COMPLETED
            };

            static void complete(void* opaque, Outcome&& outcome) noexcept
            {
                auto& self = *static_cast<State*>(opaque);
                const auto previous = self.phase_.exchange(EPhase::COMPLETED, std::memory_order_acq_rel);
                if (previous == EPhase::COMPLETED)
                    std::terminate();

                if (!outcome)
                {
                    stdexec::set_error(std::move(self.receiver_), std::move(outcome.error()));
                    return;
                }
                if constexpr (std::is_void_v<Value>)
                    stdexec::set_value(std::move(self.receiver_));
                else
                    stdexec::set_value(std::move(self.receiver_), std::move(*outcome));
            }

            lux::async::OperationPort<Operation> port_;
            Operation operation_;
            lux::async::SubmitOptions options_{};
            Receiver receiver_;
            std::atomic<EPhase> phase_{EPhase::SUBMITTING};
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
        {
            return State<std::decay_t<Receiver>>{
                std::move(port_),
                std::move(operation_),
                options_,
                std::forward<Receiver>(receiver)
            };
        }

        [[nodiscard]] stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

    private:
        lux::async::OperationPort<Operation> port_;
        Operation operation_;
        lux::async::SubmitOptions options_{};
    };

    template <lux::async::Operation Operation>
    [[nodiscard]] auto portSender(
        lux::async::OperationPort<Operation> port,
        Operation operation,
        lux::async::SubmitOptions options = {}
    ) noexcept
    {
        return PortSender<Operation>{std::move(port), std::move(operation), options};
    }
}
