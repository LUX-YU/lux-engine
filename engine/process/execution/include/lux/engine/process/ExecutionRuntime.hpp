#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/process/Timer.hpp>
#include <lux/engine/process/visibility.h>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

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
        MAIN_QUEUE_NOT_DRAINED,
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
        std::size_t main_queue_capacity{};
        TimerQueueConfig timer{};
        std::optional<BlockingSchedulerConfig> blocking;
    };

    namespace detail
    {
        struct ExecutionState;

        enum class EExecutionQueue : std::uint8_t
        {
            CPU,
            MAIN,
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

        template<EExecutionQueue Queue>
        class ScheduleSender;
    } // namespace detail

    class CpuScheduler final
    {
    public:
        CpuScheduler() noexcept = default;

        [[nodiscard]] detail::ScheduleSender<detail::EExecutionQueue::CPU> schedule() const noexcept;

        [[nodiscard]] stdexec::forward_progress_guarantee
        query(stdexec::get_forward_progress_guarantee_t) const noexcept
        {
            return stdexec::forward_progress_guarantee::parallel;
        }

        [[nodiscard]] CpuScheduler
        query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
        {
            return *this;
        }

        [[nodiscard]] bool operator==(const CpuScheduler& other) const noexcept
        {
            return identity_ == other.identity_;
        }

    private:
        friend class ExecutionRuntime;
        template<detail::EExecutionQueue>
        friend class detail::ScheduleSender;

        explicit CpuScheduler(const std::shared_ptr<detail::ExecutionState>& state) noexcept
            : state_(state), identity_(state.get())
        {
        }

        std::weak_ptr<detail::ExecutionState> state_;
        const void* identity_{};
    };

    class MainScheduler final
    {
    public:
        MainScheduler() noexcept = default;

        [[nodiscard]] detail::ScheduleSender<detail::EExecutionQueue::MAIN> schedule() const noexcept;

        [[nodiscard]] stdexec::forward_progress_guarantee
        query(stdexec::get_forward_progress_guarantee_t) const noexcept
        {
            return stdexec::forward_progress_guarantee::weakly_parallel;
        }

        [[nodiscard]] MainScheduler
        query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
        {
            return *this;
        }

        [[nodiscard]] bool operator==(const MainScheduler& other) const noexcept
        {
            return identity_ == other.identity_;
        }

    private:
        friend class ExecutionRuntime;
        template<detail::EExecutionQueue>
        friend class detail::ScheduleSender;

        explicit MainScheduler(const std::shared_ptr<detail::ExecutionState>& state) noexcept
            : state_(state), identity_(state.get())
        {
        }

        std::weak_ptr<detail::ExecutionState> state_;
        const void* identity_{};
    };

    class BlockingScheduler final
    {
    public:
        BlockingScheduler() noexcept = default;

        [[nodiscard]] detail::ScheduleSender<detail::EExecutionQueue::BLOCKING> schedule() const noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return identity_ != nullptr && !state_.expired();
        }

        [[nodiscard]] stdexec::forward_progress_guarantee
        query(stdexec::get_forward_progress_guarantee_t) const noexcept
        {
            return stdexec::forward_progress_guarantee::parallel;
        }

        [[nodiscard]] BlockingScheduler
        query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
        {
            return *this;
        }

        [[nodiscard]] bool operator==(const BlockingScheduler& other) const noexcept
        {
            return identity_ == other.identity_;
        }

    private:
        friend class ExecutionRuntime;
        template<detail::EExecutionQueue>
        friend class detail::ScheduleSender;

        explicit BlockingScheduler(const std::shared_ptr<detail::ExecutionState>& state) noexcept
            : state_(state), identity_(state.get())
        {
        }

        std::weak_ptr<detail::ExecutionState> state_;
        const void* identity_{};
    };

    namespace detail
    {
        template<EExecutionQueue Queue>
        class ScheduleSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(),
                stdexec::set_error_t(EExecutionError),
                stdexec::set_stopped_t()
            >;
            using Scheduler = std::conditional_t<
                Queue == EExecutionQueue::CPU,
                CpuScheduler,
                std::conditional_t<Queue == EExecutionQueue::MAIN, MainScheduler, BlockingScheduler>
            >;

            class Env final
            {
            public:
                explicit Env(Scheduler scheduler) noexcept : scheduler_(std::move(scheduler))
                {
                }

                template<class Completion>
                [[nodiscard]] Scheduler query(stdexec::get_completion_scheduler_t<Completion>) const noexcept
                {
                    return scheduler_;
                }

            private:
                Scheduler scheduler_;
            };

            ScheduleSender() noexcept = default;

            [[nodiscard]] Env get_env() const noexcept
            {
                return Env{scheduler_};
            }

            template<class Receiver>
            class Operation final : private ScheduleRequest
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
                    }
                };

                using StopCallback = stdexec::stop_callback_for_t<StopToken, Cancel>;

                Operation(std::weak_ptr<ExecutionState> state, Receiver receiver)
                    : state_weak_(std::move(state)), receiver_(std::move(receiver))
                {
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

                    state_ = state_weak_.lock();
                    if (!state_)
                    {
                        stdexec::set_error(std::move(receiver_), EExecutionError::STOPPING);
                        return;
                    }

                    try
                    {
                        stop_callback_.emplace(token, Cancel{this});
                    }
                    catch (const std::bad_alloc&)
                    {
                        state_.reset();
                        stdexec::set_error(std::move(receiver_), EExecutionError::ALLOCATION_FAILURE);
                        return;
                    }

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
                    auto& self = *static_cast<Operation*>(request);
                    const bool is_stopped = stopped || self.cancel_requested.load(std::memory_order_acquire);
                    auto state = std::move(self.state_);
                    self.stop_callback_.reset();
                    if (is_stopped)
                        stdexec::set_stopped(std::move(self.receiver_));
                    else
                        stdexec::set_value(std::move(self.receiver_));
                }

                std::weak_ptr<ExecutionState> state_weak_;
                std::shared_ptr<ExecutionState> state_;
                Receiver receiver_;
                std::optional<StopCallback> stop_callback_;
            };

            template<class Receiver>
            [[nodiscard]] Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) const
            {
                return Operation<std::decay_t<Receiver>>{state_, std::forward<Receiver>(receiver)};
            }

        private:
            friend class CpuScheduler;
            friend class MainScheduler;
            friend class BlockingScheduler;

            explicit ScheduleSender(Scheduler scheduler) noexcept
                : state_(scheduler.state_), scheduler_(std::move(scheduler))
            {
            }

            std::weak_ptr<ExecutionState> state_;
            Scheduler scheduler_;
        };
    } // namespace detail

    inline detail::ScheduleSender<detail::EExecutionQueue::CPU> CpuScheduler::schedule() const noexcept
    {
        return detail::ScheduleSender<detail::EExecutionQueue::CPU>{*this};
    }

    inline detail::ScheduleSender<detail::EExecutionQueue::MAIN> MainScheduler::schedule() const noexcept
    {
        return detail::ScheduleSender<detail::EExecutionQueue::MAIN>{*this};
    }

    inline detail::ScheduleSender<detail::EExecutionQueue::BLOCKING> BlockingScheduler::schedule() const noexcept
    {
        return detail::ScheduleSender<detail::EExecutionQueue::BLOCKING>{*this};
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
        [[nodiscard]] MainScheduler main() const noexcept;
        [[nodiscard]] TimerClient timer() const noexcept;
        [[nodiscard]] lux::cxx::expected<BlockingScheduler, EExecutionError> blocking() const noexcept;

        [[nodiscard]] lux::cxx::expected<std::size_t, EExecutionError>
        drainMain(std::size_t budget = static_cast<std::size_t>(-1)) noexcept;

        void requestStop() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> join() noexcept;

    private:
        ExecutionRuntime(std::shared_ptr<detail::ExecutionState> state, TimerQueue timer) noexcept;

        std::shared_ptr<detail::ExecutionState> state_;
        TimerQueue timer_;
    };

    static_assert(stdexec::scheduler<CpuScheduler>);
    static_assert(stdexec::scheduler<MainScheduler>);
    static_assert(stdexec::scheduler<BlockingScheduler>);
} // namespace lux::process
