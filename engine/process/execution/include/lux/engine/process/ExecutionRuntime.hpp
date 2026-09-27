#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/process/Timer.hpp>
#include <lux/engine/process/Task.hpp>
#include <lux/engine/process/visibility.h>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::process
{
    enum class EExecutionError : std::uint8_t
    {
        INVALID_ARGUMENT,
        INVALID_STATE,
        CAPACITY_EXCEEDED,
        STOPPING,
        ALLOCATION_FAILURE,
        WORKER_CREATION_FAILURE,
        BACKEND_FAILURE,
        WRONG_THREAD,
        WORK_PENDING,
        ALREADY_JOINED,
        CAPABILITY_UNAVAILABLE,
    };

    struct BlockingSchedulerConfig final
    {
        std::size_t concurrency{};
        std::size_t queue_capacity{};
    };

    struct ExecutionRuntimeConfig final
    {
        std::size_t cpu_concurrency{};
        std::size_t cpu_queue_capacity{};
        std::size_t task_capacity{1024};
        TimerQueueConfig timer{};
        std::optional<BlockingSchedulerConfig> blocking;
        std::size_t task_history_capacity{256};
    };

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

        [[nodiscard]] LUX_PROCESS_EXECUTION_PUBLIC ScheduleSubmitResult submitSchedule(
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

    class LUX_PROCESS_EXECUTION_PUBLIC ExecutionRuntime final
    {
    public:
        using CreateResult = lux::cxx::expected<ExecutionRuntime, EExecutionError>;

        [[nodiscard]] static CreateResult create(ExecutionRuntimeConfig config) noexcept;

        ~ExecutionRuntime() noexcept;
        ExecutionRuntime(ExecutionRuntime&& other) noexcept;
        ExecutionRuntime& operator=(ExecutionRuntime&& other) noexcept;
        ExecutionRuntime(const ExecutionRuntime&) = delete;
        ExecutionRuntime& operator=(const ExecutionRuntime&) = delete;

        [[nodiscard]] CpuScheduler cpu() const noexcept;
        // Owner-thread admission fact, before shutdown/join mutates worker storage.
        [[nodiscard]] std::size_t cpuConcurrency() const noexcept;
        [[nodiscard]] TimerClient timer() const noexcept;
        [[nodiscard]] lux::cxx::expected<BlockingScheduler, EExecutionError> blocking() const noexcept;

        template <class Factory, class Completion>
        [[nodiscard]] lux::cxx::expected<Task, EExecutionError> submit(
            TaskOptions options,
            Factory&& make_sender,
            Completion&& completed
        ) noexcept
        {
            using Sender = std::invoke_result_t<Factory, TaskReporter>;
            using Operation = detail::TTaskOperation<Sender, std::decay_t<Completion>>;
            static_assert(std::is_nothrow_invocable_v<Factory, TaskReporter>);
            auto admitted = detail::admitTask(*tasks_, std::move(options));
            if (!admitted)
                return lux::cxx::unexpected(admitted.error());
            Task task{*admitted};
            TaskReporter reporter{*admitted};
            auto sender = std::invoke(std::forward<Factory>(make_sender), reporter);
            detail::startTask(
                **admitted,
                std::make_unique<Operation>(
                    **admitted,
                    reporter,
                    std::move(sender),
                    std::forward<Completion>(completed)
                )
            );
            return task;
        }

        // Collection never calls a task's business completion. Dispatch is an explicit owner boundary.
        [[nodiscard]] lux::cxx::expected<std::size_t, EExecutionError> collectCompletions() noexcept;
        [[nodiscard]] lux::cxx::expected<std::size_t, EExecutionError> dispatchTaskEvents() noexcept;
        [[nodiscard]] std::optional<TaskInfo> taskInfo(TaskId id) const noexcept;
        [[nodiscard]] std::vector<TaskInfo> taskInfos() const noexcept;
        [[nodiscard]] bool requestStop(TaskId id) noexcept;
        using TaskObserver = void (*)(void*, std::span<const TaskId>, bool resync) noexcept;
        void setTaskObserver(void* owner, TaskObserver observer) noexcept;

        // Runs the batch ready at entry, outside queue locks. New completions/work wait for the next call.
        [[nodiscard]] std::uint64_t wakeEpoch() const noexcept;
        [[nodiscard]] bool hasPendingWork() const noexcept;
        void wake() noexcept;
        void setWake(void (*wake)() noexcept) noexcept;
        void waitForWork(
            std::uint64_t observed,
            std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> waitUntil(
            void* context,
            bool (*ready)(void*) noexcept
        ) noexcept;

        template <class Predicate>
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> waitUntil(Predicate&& ready) noexcept
        {
            return waitUntil(std::addressof(ready), [](void* value) noexcept {
                return (*static_cast<std::remove_reference_t<Predicate>*>(value))();
            });
        }

        void requestStop() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> join() noexcept;

    private:
        friend class CompletionWork;
        friend class TaskScope;
        friend struct detail::TaskRuntime;
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> validateWait() const noexcept;
        ExecutionRuntime(
            std::shared_ptr<detail::ExecutionState> state,
            TimerQueue timer,
            std::size_t task_capacity,
            std::size_t history_capacity
        ) noexcept;
        void finishShutdown() noexcept;

        std::shared_ptr<detail::ExecutionState> state_;
        TimerQueue timer_;
        std::unique_ptr<detail::TaskRuntime> tasks_;
    };

    static_assert(stdexec::scheduler<CpuScheduler>);
    static_assert(stdexec::scheduler<BlockingScheduler>);
} // namespace lux::process
