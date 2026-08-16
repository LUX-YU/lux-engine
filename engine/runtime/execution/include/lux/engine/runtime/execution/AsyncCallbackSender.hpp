#pragma once
/**
 * @file AsyncCallbackSender.hpp
 * @brief Stop-safe stdexec adapter for one-shot callback/RPC APIs.
 *
 * This is an adapter at a protocol boundary, not a second task runtime. The
 * callback producer must settle exactly once and return an explicit handoff
 * action for the case where downstream cancellation wins.
 */

#include <lux/cxx/core/move_only_function.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace lux::exec
{
    struct AsyncCallbackError final
    {
        std::string reason;
    };

    class AsyncStopAction final
    {
    public:
        AsyncStopAction() noexcept = default;

        template <class Function>
            requires (!std::is_same_v<
                std::remove_cvref_t<Function>,
                AsyncStopAction>)
        explicit AsyncStopAction(Function&& function)
            : function_(std::forward<Function>(function))
        {
            static_assert(
                std::is_nothrow_invocable_v<std::decay_t<Function>&>,
                "AsyncStopAction must be noexcept invocable");
        }

        AsyncStopAction(const AsyncStopAction&) = delete;
        AsyncStopAction& operator=(const AsyncStopAction&) = delete;
        AsyncStopAction(AsyncStopAction&&) noexcept = default;
        AsyncStopAction& operator=(AsyncStopAction&&) noexcept = default;

        void run() noexcept
        {
            if (!function_)
                return;
            auto function = std::move(function_);
            function();
        }

    private:
        lux::cxx::move_only_function<void()> function_;
    };

    namespace detail
    {
        template <class Value, class Init>
        struct AsyncCallbackSender final
        {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(Value),
                stdexec::set_error_t(AsyncCallbackError),
                stdexec::set_stopped_t()>;

            Init init;

            [[nodiscard]] stdexec::env<> get_env() const noexcept
            {
                return {};
            }

            template <class Receiver>
            struct Operation final
            {
                using operation_state_concept = stdexec::operation_state_t;

                enum class EPhase : std::uint8_t
                {
                    PRE_START,
                    STOP_BEFORE_START,
                    STARTING,
                    STOP_DURING_START,
                    ACTIVE,
                    VALUE,
                    ERROR,
                    STOPPED
                };

                struct State;

                struct Completer final
                {
                    std::shared_ptr<State> state;

                    explicit Completer(
                        std::shared_ptr<State> value) noexcept
                        : state(std::move(value))
                    {}

                    Completer(const Completer&) = delete;
                    Completer& operator=(const Completer&) = delete;
                    Completer(Completer&&) noexcept = default;
                    Completer& operator=(Completer&&) noexcept = default;

                    void complete(Value value) && noexcept
                    {
                        state->completeValue(std::move(value));
                    }

                    void fail(std::string reason) && noexcept
                    {
                        state->completeError(std::move(reason));
                    }
                };

                using StopToken = stdexec::stop_token_of_t<
                    stdexec::env_of_t<Receiver>>;

                struct State final : std::enable_shared_from_this<State>
                {
                    struct ForwardStop final
                    {
                        std::weak_ptr<State> state;

                        void operator()() noexcept
                        {
                            if (auto locked = state.lock())
                                locked->requestStop();
                        }
                    };

                    using StopCallback = stdexec::stop_callback_for_t<
                        StopToken,
                        ForwardStop>;

                    template <class OtherReceiver>
                    explicit State(OtherReceiver&& value)
                        : receiver(std::forward<OtherReceiver>(value))
                    {}

                    void armStop() noexcept
                    {
                        stop_callback.emplace(
                            stdexec::get_stop_token(
                                stdexec::get_env(receiver)),
                            ForwardStop{this->weak_from_this()});
                    }

                    [[nodiscard]] bool beginStart() noexcept
                    {
                        auto phase_value = phase.load(
                            std::memory_order_acquire);
                        for (;;)
                        {
                            if (phase_value == EPhase::PRE_START)
                            {
                                if (phase.compare_exchange_weak(
                                        phase_value,
                                        EPhase::STARTING,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
                                    return true;
                                continue;
                            }
                            if (phase_value == EPhase::STOP_BEFORE_START)
                            {
                                if (phase.compare_exchange_weak(
                                        phase_value,
                                        EPhase::STOPPED,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
                                {
                                    finishStoppedWithoutAction();
                                    return false;
                                }
                                continue;
                            }
                            return false;
                        }
                    }

                    void activate(AsyncStopAction action) noexcept
                    {
                        stop_action = std::move(action);
                        auto phase_value = phase.load(
                            std::memory_order_acquire);
                        for (;;)
                        {
                            if (phase_value == EPhase::STARTING)
                            {
                                if (phase.compare_exchange_weak(
                                        phase_value,
                                        EPhase::ACTIVE,
                                        std::memory_order_release,
                                        std::memory_order_acquire))
                                    return;
                                continue;
                            }
                            if (phase_value == EPhase::STOP_DURING_START)
                            {
                                if (phase.compare_exchange_weak(
                                        phase_value,
                                        EPhase::STOPPED,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
                                {
                                    finishStoppedWithAction();
                                    return;
                                }
                                continue;
                            }
                            stop_action = {};
                            return;
                        }
                    }

                    void completeValue(Value value) noexcept
                    {
                        const auto source = claimCompletion(EPhase::VALUE);
                        if (!source)
                            return;
                        disarmStop(*source);
                        stdexec::set_value(
                            std::move(receiver),
                            std::move(value));
                    }

                    void completeError(std::string reason) noexcept
                    {
                        const auto source = claimCompletion(EPhase::ERROR);
                        if (!source)
                            return;
                        disarmStop(*source);
                        stdexec::set_error(
                            std::move(receiver),
                            AsyncCallbackError{std::move(reason)});
                    }

                    void requestStop() noexcept
                    {
                        auto phase_value = phase.load(
                            std::memory_order_acquire);
                        for (;;)
                        {
                            switch (phase_value)
                            {
                            case EPhase::PRE_START:
                                if (phase.compare_exchange_weak(
                                        phase_value,
                                        EPhase::STOP_BEFORE_START,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
                                    return;
                                break;
                            case EPhase::STARTING:
                                if (phase.compare_exchange_weak(
                                        phase_value,
                                        EPhase::STOP_DURING_START,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
                                    return;
                                break;
                            case EPhase::ACTIVE:
                                if (phase.compare_exchange_weak(
                                        phase_value,
                                        EPhase::STOPPED,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
                                {
                                    finishStoppedWithAction();
                                    return;
                                }
                                break;
                            default:
                                return;
                            }
                        }
                    }

                    Receiver receiver;
                    std::atomic<EPhase> phase{EPhase::PRE_START};
                    AsyncStopAction stop_action;
                    std::optional<StopCallback> stop_callback;

                private:
                    [[nodiscard]] std::optional<EPhase> claimCompletion(
                        EPhase terminal) noexcept
                    {
                        auto phase_value = phase.load(
                            std::memory_order_acquire);
                        for (;;)
                        {
                            switch (phase_value)
                            {
                            case EPhase::STARTING:
                            case EPhase::STOP_DURING_START:
                            case EPhase::ACTIVE:
                                if (phase.compare_exchange_weak(
                                        phase_value,
                                        terminal,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
                                    return phase_value;
                                break;
                            default:
                                return std::nullopt;
                            }
                        }
                    }

                    void disarmStop(EPhase source) noexcept
                    {
                        stop_callback.reset();
                        if (source == EPhase::ACTIVE)
                            stop_action = {};
                    }

                    void finishStoppedWithoutAction() noexcept
                    {
                        stop_callback.reset();
                        stdexec::set_stopped(std::move(receiver));
                    }

                    void finishStoppedWithAction() noexcept
                    {
                        stop_callback.reset();
                        stop_action.run();
                        stdexec::set_stopped(std::move(receiver));
                    }
                };

                Init init;
                std::shared_ptr<State> state;

                void start() & noexcept
                {
                    static_assert(
                        std::is_nothrow_move_constructible_v<Value>,
                        "callback sender value must be nothrow movable");
                    static_assert(
                        std::is_nothrow_invocable_r_v<
                            AsyncStopAction,
                            Init&&,
                            Completer>,
                        "callback init must be noexcept and return "
                        "AsyncStopAction");

                    auto keep_alive = state;
                    auto start_callback = std::move(init);
                    keep_alive->armStop();
                    if (!keep_alive->beginStart())
                        return;
                    auto action = std::move(start_callback)(
                        Completer{keep_alive});
                    keep_alive->activate(std::move(action));
                }
            };

            template <class Receiver>
            [[nodiscard]] Operation<std::decay_t<Receiver>> connect(
                Receiver&& receiver) &&
            {
                using State = typename Operation<
                    std::decay_t<Receiver>>::State;
                return {
                    std::move(init),
                    std::make_shared<State>(
                        std::forward<Receiver>(receiver))};
            }
        };
    }

    template <class Value, class Init>
    [[nodiscard]] auto callbackSender(Init&& init)
    {
        return detail::AsyncCallbackSender<
            Value,
            std::decay_t<Init>>{
                std::forward<Init>(init)};
    }
}
