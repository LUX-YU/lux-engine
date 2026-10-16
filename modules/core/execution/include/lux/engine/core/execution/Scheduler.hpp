#pragma once

#include <lux/engine/core/execution/ExecutionError.hpp>
#include <stdexec/execution.hpp>
#include <atomic>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lux::execution
{
    namespace detail
    {
        struct ExecutionState;

        enum class EExecutionQueue : std::uint8_t
        {
            CPU,
            BLOCKING,
        };

        struct ScheduleRequest
        {
            std::atomic_bool cancel_requested{false};
            void (*complete)(ScheduleRequest*, bool stopped) noexcept {};
        };

        using ScheduleSubmitResult = lux::cxx::expected<void, EExecutionError>;

        [[nodiscard]] LUX_CORE_EXECUTION_PUBLIC ScheduleSubmitResult submitSchedule(
            const std::shared_ptr<ExecutionState>& state,
            EExecutionQueue queue,
            ScheduleRequest& request
        ) noexcept;

        template <EExecutionQueue Queue> class TScheduleSender;
    } // namespace detail

    class CpuScheduler final
    {
    public:
        CpuScheduler() noexcept = default;

        [[nodiscard]] detail::TScheduleSender<detail::EExecutionQueue::CPU> schedule() const noexcept;

        [[nodiscard]] stdexec::forward_progress_guarantee query(stdexec::get_forward_progress_guarantee_t
        ) const noexcept
        {
            return stdexec::forward_progress_guarantee::parallel;
        }

        [[nodiscard]] CpuScheduler query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
        {
            return *this;
        }

        [[nodiscard]] bool operator==(const CpuScheduler& other) const noexcept
        {
            return identity_ == other.identity_;
        }

    private:
        friend class ExecutionRuntime;
        template <detail::EExecutionQueue> friend class detail::TScheduleSender;

        explicit CpuScheduler(const std::shared_ptr<detail::ExecutionState>& state) noexcept
            : state_(state), identity_(state.get())
        {}

        std::weak_ptr<detail::ExecutionState> state_;
        const void* identity_{};
    };

    class BlockingScheduler final
    {
    public:
        BlockingScheduler() noexcept = default;

        [[nodiscard]] detail::TScheduleSender<detail::EExecutionQueue::BLOCKING> schedule() const noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return identity_ != nullptr && !state_.expired();
        }

        [[nodiscard]] stdexec::forward_progress_guarantee query(stdexec::get_forward_progress_guarantee_t
        ) const noexcept
        {
            return stdexec::forward_progress_guarantee::parallel;
        }

        [[nodiscard]] BlockingScheduler query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
        {
            return *this;
        }

        [[nodiscard]] bool operator==(const BlockingScheduler& other) const noexcept
        {
            return identity_ == other.identity_;
        }

    private:
        friend class ExecutionRuntime;
        template <detail::EExecutionQueue> friend class detail::TScheduleSender;

        explicit BlockingScheduler(const std::shared_ptr<detail::ExecutionState>& state) noexcept
            : state_(state), identity_(state.get())
        {}

        std::weak_ptr<detail::ExecutionState> state_;
        const void* identity_{};
    };

    namespace detail
    {
        template <EExecutionQueue Queue> class TScheduleSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(),
                stdexec::set_error_t(EExecutionError),
                stdexec::set_stopped_t()>;
            using Scheduler = std::conditional_t<Queue == EExecutionQueue::CPU, CpuScheduler, BlockingScheduler>;

            class Env final
            {
            public:
                explicit Env(Scheduler scheduler) noexcept : scheduler_(std::move(scheduler)) {}

                template <class Completion>
                [[nodiscard]] Scheduler query(stdexec::get_completion_scheduler_t<Completion>) const noexcept
                {
                    return scheduler_;
                }

            private:
                Scheduler scheduler_;
            };

            TScheduleSender() noexcept = default;

            [[nodiscard]] Env get_env() const noexcept
            {
                return Env{scheduler_};
            }

            template <class Receiver> class TOperation final : private ScheduleRequest
            {
            public:
                using operation_state_concept = stdexec::operation_state_t;
                using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

                struct Cancel final
                {
                    TOperation* operation{};

                    void operator()() noexcept
                    {
                        operation->cancel_requested.store(true, std::memory_order_release);
                    }
                };

                using StopCallback = stdexec::stop_callback_for_t<StopToken, Cancel>;

                TOperation(std::weak_ptr<ExecutionState> state, Receiver receiver)
                    : state_weak_(std::move(state)), receiver_(std::move(receiver))
                {
                    this->complete = &TOperation::completeRequest;
                }

                TOperation(const TOperation&) = delete;
                TOperation& operator=(const TOperation&) = delete;
                TOperation(TOperation&&) = delete;
                TOperation& operator=(TOperation&&) = delete;

                void start() & noexcept
                {
                    const auto token = stdexec::get_stop_token(stdexec::get_env(receiver_));
                    if (token.stop_requested())
                    {
                        stdexec::set_stopped(std::move(receiver_));
                        return;
                    }

                    state_ = state_weak_.lock();
                    if (!state_)
                    {
                        stdexec::set_error(std::move(receiver_), EExecutionError::STOPPING);
                        return;
                    }

                    stop_callback_.emplace(token, Cancel{this});

                    auto submitted = submitSchedule(state_, Queue, static_cast<ScheduleRequest&>(*this));
                    if (!submitted)
                    {
                        stop_callback_.reset();
                        state_.reset();
                        stdexec::set_error(std::move(receiver_), submitted.error());
                    }
                }

            private:
                static void completeRequest(ScheduleRequest* request, bool stopped) noexcept
                {
                    auto& self = *static_cast<TOperation*>(request);
                    const bool is_stopped = stopped || self.cancel_requested.load(std::memory_order_acquire);
                    auto state = std::move(self.state_);
                    self.stop_callback_.reset();
                    if (is_stopped)
                    {
                        stdexec::set_stopped(std::move(self.receiver_));
                    }
                    else
                    {
                        stdexec::set_value(std::move(self.receiver_));
                    }
                }

                std::weak_ptr<ExecutionState> state_weak_;
                std::shared_ptr<ExecutionState> state_;
                Receiver receiver_;
                std::optional<StopCallback> stop_callback_;
            };

            template <class Receiver>
            [[nodiscard]] TOperation<std::decay_t<Receiver>> connect(Receiver&& receiver) const
            {
                return TOperation<std::decay_t<Receiver>>{state_, std::forward<Receiver>(receiver)};
            }

        private:
            friend class CpuScheduler;
            friend class BlockingScheduler;

            explicit TScheduleSender(Scheduler scheduler) noexcept
                : state_(scheduler.state_), scheduler_(std::move(scheduler))
            {}

            std::weak_ptr<ExecutionState> state_;
            Scheduler scheduler_;
        };
    } // namespace detail

    inline detail::TScheduleSender<detail::EExecutionQueue::CPU> CpuScheduler::schedule() const noexcept
    {
        return detail::TScheduleSender<detail::EExecutionQueue::CPU>{*this};
    }

    inline detail::TScheduleSender<detail::EExecutionQueue::BLOCKING> BlockingScheduler::schedule() const noexcept
    {
        return detail::TScheduleSender<detail::EExecutionQueue::BLOCKING>{*this};
    }

}
