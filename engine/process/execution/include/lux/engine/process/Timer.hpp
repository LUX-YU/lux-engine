#pragma once
/**
 * @file Timer.hpp
 * @brief Bounded cancellable Timer sender backed by one owner thread.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/process/visibility.h>

#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lux::process
{
    enum class ETimerError : std::uint8_t
    {
        INVALID_ARGUMENT,
        CAPACITY_EXCEEDED,
        STOPPING,
        ALLOCATION_FAILURE,
        WORKER_CREATION_FAILURE,
        BACKEND_FAILURE
    };

    struct TimerQueueConfig final
    {
        std::size_t capacity{};
    };

    namespace detail
    {
        struct TimerState;

        enum class ETimerSubmitStatus : std::uint8_t
        {
            SUBMITTED,
            STOPPED
        };

        struct TimerRequest
        {
            using Clock = std::chrono::steady_clock;

            Clock::duration delay{};
            Clock::time_point deadline{};
            std::size_t heap_index{static_cast<std::size_t>(-1)};
            std::atomic_bool cancel_requested{false};
            bool queued{};
            bool cancellation_queued{};
            void (*complete)(TimerRequest*, bool stopped) noexcept {};
        };

        using TimerSubmitResult = lux::cxx::expected<ETimerSubmitStatus, ETimerError>;

        [[nodiscard]] LUX_PROCESS_EXECUTION_PUBLIC TimerSubmitResult submitTimer(
            const std::shared_ptr<TimerState>& state,
            TimerRequest& request
        ) noexcept;

        LUX_PROCESS_EXECUTION_PUBLIC void cancelTimer(TimerState* state, TimerRequest& request) noexcept;
        void stopTimerState(const std::shared_ptr<TimerState>& state) noexcept;
    }

    class TimerSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::
            completion_signatures<stdexec::set_value_t(), stdexec::set_error_t(ETimerError), stdexec::set_stopped_t()>;

        TimerSender() noexcept = default;

        template <class Receiver> class Operation final : private detail::TimerRequest
        {
        public:
            using operation_state_concept = stdexec::operation_state_t;
            using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

            struct Cancel final
            {
                Operation* operation{};

                void operator()() noexcept
                {
                    operation->cancel_requested.store(true, std::memory_order_release);
                    auto* state = operation->state_.load(std::memory_order_acquire);
                    if (state != nullptr)
                        detail::cancelTimer(state, static_cast<detail::TimerRequest&>(*operation));
                }
            };

            using StopCallback = stdexec::stop_callback_for_t<StopToken, Cancel>;

            Operation(std::weak_ptr<detail::TimerState> state,
                      std::chrono::steady_clock::duration delay,
                      Receiver receiver)
                : state_weak_(std::move(state)), receiver_(std::move(receiver))
            {
                this->delay = delay;
                this->complete = &Operation::completeRequest;
            }

            Operation(const Operation&) = delete;
            Operation& operator=(const Operation&) = delete;
            Operation(Operation&&) = delete;
            Operation& operator=(Operation&&) = delete;

            void start() & noexcept
            {
                const auto token = stdexec::get_stop_token(stdexec::get_env(receiver_));
                if (token.stop_requested())
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }

                auto state = state_weak_.lock();
                if (!state)
                {
                    stdexec::set_error(std::move(receiver_), ETimerError::STOPPING);
                    return;
                }

                state_.store(state.get(), std::memory_order_release);
                stop_callback_.emplace(token, Cancel{this});

                const auto now = detail::TimerRequest::Clock::now();
                const auto maximum_delay = detail::TimerRequest::Clock::time_point::max() - now;
                if (this->delay <= detail::TimerRequest::Clock::duration::zero())
                    this->deadline = now;
                else if (this->delay >= maximum_delay)
                    this->deadline = detail::TimerRequest::Clock::time_point::max();
                else
                    this->deadline = now + this->delay;

                const auto submitted = detail::submitTimer(state, static_cast<detail::TimerRequest&>(*this));
                if (!submitted)
                {
                    stop_callback_.reset();
                    state_.store(nullptr, std::memory_order_release);
                    stdexec::set_error(std::move(receiver_), submitted.error());
                    return;
                }
                if (*submitted == detail::ETimerSubmitStatus::STOPPED)
                {
                    stop_callback_.reset();
                    state_.store(nullptr, std::memory_order_release);
                    stdexec::set_stopped(std::move(receiver_));
                }
            }

        private:
            static void completeRequest(detail::TimerRequest* request, bool stopped) noexcept
            {
                auto& self = *static_cast<Operation*>(request);
                self.stop_callback_.reset();
                self.state_.store(nullptr, std::memory_order_release);
                if (stopped)
                    stdexec::set_stopped(std::move(self.receiver_));
                else
                    stdexec::set_value(std::move(self.receiver_));
            }

            std::weak_ptr<detail::TimerState> state_weak_;
            std::atomic<detail::TimerState*> state_{nullptr};
            Receiver receiver_;
            std::optional<StopCallback> stop_callback_;
        };

        template <class Receiver> [[nodiscard]] Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
        {
            return Operation<std::decay_t<Receiver>>{std::move(state_), delay_, std::forward<Receiver>(receiver)};
        }

        [[nodiscard]] stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

    private:
        friend class TimerClient;

        TimerSender(std::weak_ptr<detail::TimerState> state, std::chrono::steady_clock::duration delay) noexcept
            : state_(std::move(state)), delay_(delay)
        {}

        std::weak_ptr<detail::TimerState> state_;
        std::chrono::steady_clock::duration delay_{};
    };

    class LUX_PROCESS_EXECUTION_PUBLIC TimerClient final
    {
    public:
        TimerClient() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return !state_.expired();
        }

        [[nodiscard]] TimerSender after(std::chrono::steady_clock::duration delay) const noexcept
        {
            return TimerSender{state_, delay};
        }

    private:
        friend class TimerQueue;

        explicit TimerClient(const std::shared_ptr<detail::TimerState>& state) noexcept : state_(state) {}

        std::weak_ptr<detail::TimerState> state_;
    };

    class LUX_PROCESS_EXECUTION_PUBLIC TimerQueue final
    {
    public:
        using CreateResult = lux::cxx::expected<TimerQueue, ETimerError>;

        [[nodiscard]] static CreateResult create(TimerQueueConfig config) noexcept;

        ~TimerQueue();

        TimerQueue(const TimerQueue&) = delete;
        TimerQueue& operator=(const TimerQueue&) = delete;
        TimerQueue(TimerQueue&&) noexcept;
        TimerQueue& operator=(TimerQueue&&) noexcept;

        [[nodiscard]] TimerClient client() const noexcept
        {
            return TimerClient{state_};
        }

        /// Stops admission, completes queued requests as stopped and joins the
        /// owner thread. Do not destroy the queue from one of its receivers.
        void requestStop() noexcept;

    private:
        explicit TimerQueue(std::shared_ptr<detail::TimerState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<detail::TimerState> state_;
    };
}
