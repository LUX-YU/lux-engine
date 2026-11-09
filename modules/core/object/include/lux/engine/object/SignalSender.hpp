#pragma once

#include <exec/completion_behavior.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <stdexec/execution.hpp>
#include <atomic>
#include <optional>
#include <variant>

namespace lux::object
{
    // One emission, source close, or cancellation. Start on the ObjectRuntime thread.
    // Delivery uses the process application scheduler, which preserves determined results during shutdown.
    template <class Source, class Declaring, class Payload> class SignalSender final
    {
        static_assert(std::is_void_v<Payload> || std::is_copy_constructible_v<Payload>);

        struct Environment final
        {
            [[nodiscard]] auto query(exec::get_completion_behavior_t<stdexec::set_value_t>) const noexcept
            {
                return exec::completion_behavior::asynchronous_affine;
            }
        };

        enum class ECompletion : std::uint8_t
        {
            WAITING,
            VALUE,
            SOURCE_CLOSED,
            STOPPED
        };

        using StoredPayload = std::conditional_t<std::is_void_v<Payload>, std::monostate, Payload>;

    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(Payload),
            stdexec::set_error_t(EConnectError),
            stdexec::set_stopped_t()
        >;

        SignalSender(Source& source, TSignal<Payload> Declaring::* signal) noexcept
            : source_(source), signal_(signal)
        {
        }

        [[nodiscard]] Environment get_env() const noexcept
        {
            return {};
        }

        template <class Receiver> class Operation final
        {
            using ReceiverEnvironment = stdexec::env_of_t<Receiver>;
            using StopToken = stdexec::stop_token_of_t<ReceiverEnvironment>;
            using DeliveryEnvironment = decltype(stdexec::env{
                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}},
                std::declval<ReceiverEnvironment>()
            });

            struct DeliveryReceiver final
            {
                using receiver_concept = stdexec::receiver_t;
                Operation& operation;

                [[nodiscard]] DeliveryEnvironment get_env() const noexcept
                {
                    return {
                        stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}},
                        stdexec::get_env(operation.receiver_)
                    };
                }

                void set_value() && noexcept
                {
                    operation.deliver();
                }


            };

            struct Cancel final
            {
                Operation& operation;

                void operator()() const noexcept
                {
                    operation.complete(ECompletion::STOPPED);
                }
            };

            using StopCallback = stdexec::stop_callback_for_t<StopToken, Cancel>;
            using DeliveryOperation =
                stdexec::connect_result_t<execution::RunLoop::ScheduleSender, DeliveryReceiver>;

        public:
            using operation_state_concept = stdexec::operation_state_t;

            Operation(const SignalSender& sender, Receiver receiver) noexcept
                : receiver_(std::move(receiver)), source_(sender.source_), signal_(sender.signal_),
                  delivery_(stdexec::connect(
                      stdexec::schedule(execution::ExecutionRuntime::instance().mainScheduler()),
                      DeliveryReceiver{*this}
                  ))
            {
            }

            void start() & noexcept
            {
                if (!source_.isOnAffinityThread())
                {
                    stdexec::set_error(std::move(receiver_), EConnectError::WRONG_THREAD);
                    return;
                }
                auto stop = stdexec::get_stop_token(stdexec::get_env(receiver_));
                if (stop.stop_requested())
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                auto connected = connectSignal();
                if (!connected)
                {
                    stdexec::set_error(std::move(receiver_), connected.error());
                    return;
                }
                connection_ = std::move(*connected);
                connection_.observeSourceClose(this, +[](void* value) noexcept
                {
                    static_cast<Operation*>(value)->complete(ECompletion::SOURCE_CLOSED);
                });
                stop_callback_.emplace(stop, Cancel{*this});
            }

        private:
            [[nodiscard]] LuxObject::ConnectResult connectSignal() noexcept
            {
                if constexpr (std::is_void_v<Payload>)
                {
                    return LuxObject::connect(&source_, signal_, [this]() noexcept { emitted(); });
                }
                else
                {
                    return LuxObject::connect(&source_, signal_, [this](const Payload& value) noexcept
                    {
                        emitted(value);
                    });
                }
            }

            template <class... Values> void emitted(const Values&... values) noexcept
            {
                auto expected = ECompletion::WAITING;
                if (!completion_.compare_exchange_strong(expected, ECompletion::VALUE, std::memory_order_acq_rel))
                {
                    return;
                }
                payload_.emplace(values...);
                stdexec::start(delivery_);
                // Delivery may reclaim this operation. No member access follows publication.
            }

            void complete(ECompletion completion) noexcept
            {
                auto expected = ECompletion::WAITING;
                if (!completion_.compare_exchange_strong(expected, completion, std::memory_order_acq_rel))
                {
                    return;
                }
                stdexec::start(delivery_);
            }

            void deliver() noexcept
            {
                // Cross-thread stop callbacks must return before their connected state is destroyed.
                stop_callback_.reset();
                connection_.disconnect();
                switch (completion_.load(std::memory_order_acquire))
                {
                case ECompletion::VALUE:
                    if constexpr (std::is_void_v<Payload>)
                    {
                        stdexec::set_value(std::move(receiver_));
                    }
                    else
                    {
                        stdexec::set_value(std::move(receiver_), std::move(*payload_));
                    }
                    return;
                case ECompletion::SOURCE_CLOSED:
                    stdexec::set_error(std::move(receiver_), EConnectError::OBJECT_CLOSED);
                    return;
                case ECompletion::STOPPED:
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                case ECompletion::WAITING:
                    std::terminate();
                }
            }

            Receiver receiver_;
            Source& source_;
            TSignal<Payload> Declaring::* signal_;
            Connection connection_;
            std::optional<StoredPayload> payload_;
            std::atomic<ECompletion> completion_{ECompletion::WAITING};
            DeliveryOperation delivery_;
            std::optional<StopCallback> stop_callback_;
        };

        template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
        {
            return Operation<Receiver>{*this, std::move(receiver)};
        }

    private:
        Source& source_;
        TSignal<Payload> Declaring::* signal_;
    };

    // The caller awaits the next event, not a replay. Stateful readiness queries must first
    // inspect their domain state and register this wait in the same Object-thread segment.
    template <class Source, class Declaring, class Payload>
        requires std::derived_from<Source, Declaring>
    [[nodiscard]] SignalSender<Source, Declaring, Payload>
    whenEmitted(Source& source, TSignal<Payload> Declaring::* signal) noexcept
    {
        return {source, signal};
    }
} // namespace lux::object
