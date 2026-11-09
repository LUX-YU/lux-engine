#pragma once

#include <lux/engine/core/execution/ExecutionResult.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/core/execution/detail/TaskGroup.hpp>

#include <exception>
#include <functional>
#include <type_traits>

namespace lux::execution
{
    namespace detail
    {
        LUX_CORE_EXECUTION_PUBLIC void closeTaskGroup(TaskRuntime&, const std::shared_ptr<TaskGroup>&) noexcept;

        template <class T> struct TaskGroupAllocator final
        {
            using value_type = T;
            ExecutionAllocator<T> execution;
            std::shared_ptr<TaskGroup> group;

            TaskGroupAllocator(ExecutionState& state, std::shared_ptr<TaskGroup> value) noexcept
                : execution(state), group(std::move(value))
            {
            }

            template <class U>
            TaskGroupAllocator(const TaskGroupAllocator<U>& other) noexcept
                : execution(other.execution), group(other.group)
            {
            }

            [[nodiscard]] T* allocate(std::size_t count)
            {
                return execution.allocate(count);
            }

            void deallocate(T* memory, std::size_t count) noexcept
            {
                group->active_operations.fetch_sub(1, std::memory_order_acq_rel);
                execution.deallocate(memory, count);
            }

            template <class U> [[nodiscard]] bool operator==(const TaskGroupAllocator<U>& other) const noexcept
            {
                return execution == other.execution && group == other.group;
            }
        };

        struct TaskCapacityWaiter
        {
            TaskCapacityWaiter* next{};
            TaskCapacityWaiter** previous{};
            TaskCapacityWaiter* group_next{};
            TaskCapacityWaiter** group_previous{};
            std::shared_ptr<TaskGroup> group;
            std::shared_ptr<const void> code_lifetime;
            void (*complete)(TaskCapacityWaiter*, bool stopped) noexcept {};
        };

        LUX_CORE_EXECUTION_PUBLIC void waitForTaskCapacity(TaskRuntime&, TaskCapacityWaiter&) noexcept;
    } // namespace detail

    class LUX_CORE_EXECUTION_PUBLIC TaskScope final
    {
    public:
        class CompletionSender;
        class CapacitySender;
        explicit TaskScope(ExecutionRuntime& execution) noexcept;
        ~TaskScope() noexcept;
        TaskScope(const TaskScope&) = delete;
        TaskScope& operator=(const TaskScope&) = delete;
        TaskScope(TaskScope&&) = delete;
        TaskScope& operator=(TaskScope&&) = delete;

        [[nodiscard]] ExecutionRuntime& execution() const noexcept
        {
            return execution_;
        }

        // Finite, unmonitored work. The group contributes cancellation and a stdexec
        // association; the Runtime still owns the one spawned operation and producer tail.
        template <stdexec::sender Sender>
        [[nodiscard]] cxx::expected<void, EExecutionError> spawn(Sender sender) noexcept
        {
            auto group = group_;
            if (!group->accepting.load(std::memory_order_acquire))
            {
                return cxx::unexpected(EExecutionError::STOPPING);
            }
            // Cover admission racing close and synchronous start. No group join can
            // publish completion before the accepted root has acquired its own association.
            auto submitting = group->scope.get_token().try_associate();
            if (!submitting)
            {
                return cxx::unexpected(EExecutionError::STOPPING);
            }
            auto associated = stdexec::associate(
                stdexec::write_env(std::move(sender), stdexec::prop{stdexec::get_stop_token, group->stop.get_token()}),
                group->scope.get_token()
            );
            group->active_operations.fetch_add(1, std::memory_order_acq_rel);
            auto accepted = execution_.spawnWithAllocator(
                std::move(associated),
                detail::TaskGroupAllocator<std::byte>{*execution_.state_, group}
            );
            if (!accepted)
            {
                group->active_operations.fetch_sub(1, std::memory_order_acq_rel);
            }
            return accepted;
        }

        // Start the root on Main, supplying the scheduler required by exec::task.
        // Engine dispatches outside graph traversal and EnTT callbacks; Main identity alone is insufficient.
        // This boundary must not be removed just because a caller currently happens to run on Main.
        // Main delivery, including its captures, stays inside the original root's association.
        // Once materialized, a result must be delivered even if the task group is stopping.
        template <stdexec::sender Sender, class Completion>
        [[nodiscard]] cxx::expected<void, EExecutionError> spawnOnMain(Sender sender, Completion completion) noexcept
        {
            auto delivered = stdexec::let_value(
                execution::asResult(stdexec::starts_on(execution().mainScheduler(), std::move(sender))),
                [scheduler = execution().mainScheduler(),
                 completion = std::move(completion)](auto& outcome) mutable noexcept
                {
                    using Outcome = std::remove_cvref_t<decltype(outcome)>;
                    static_assert(std::is_nothrow_invocable_v<Completion&, Outcome>);
                    static_assert(std::is_void_v<std::invoke_result_t<Completion&, Outcome>>);
                    auto delivery = stdexec::starts_on(
                        scheduler,
                        stdexec::then(stdexec::just(std::move(outcome)), std::move(completion))
                    );
                    return stdexec::write_env(
                        std::move(delivery),
                        stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                    );
                }
            );
            // let_value can report an exception while connecting its returned sender. Business
            // failures are already values; unexpected allocation/construction failure is fatal.
            auto normalized =
                stdexec::upon_error(std::move(delivered), [](std::exception_ptr) noexcept { std::terminate(); });
            return spawn(std::move(normalized));
        }

        // Accepted finite work outlives this cancellation group. Its sender explicitly chooses
        // transport/adoption threads and consumes business values before completing.
        template <class Factory>
        [[nodiscard]] lux::cxx::expected<TaskId, EExecutionError>
        submit(TaskOptions options, Factory&& make_sender) noexcept
        {
            static_assert(std::is_nothrow_invocable_v<Factory, TaskReporter>);
            auto admitted = detail::admitTask(*execution_.tasks_, std::move(options), group_);
            if (!admitted)
            {
                return lux::cxx::unexpected(admitted.error());
            }
            auto& record = **admitted;
            const auto id = detail::taskId(record);
            TaskReporter reporter{&record};
            detail::spawnTask(record, reporter, std::invoke(std::forward<Factory>(make_sender), reporter));
            return id;
        }

        void requestStop() noexcept;
        // Borrow the group's cancellation domain for child IO senders without creating a TaskRecord.
        // Register unmonitored roots with spawn(); directly awaited children share that root.
        [[nodiscard]] std::stop_token stopToken() const noexcept;
        // Thread-safe observation only: no waiting, callbacks or admission changes.
        [[nodiscard]] bool settled() const noexcept;
        // Close admission when started; complete after the accepted operation states have ended.
        // No cancellation or nested loop. The waiting operation must be outside this group.
        [[nodiscard]] CompletionSender whenSettled() noexcept;
        // Observe available Runtime task capacity without closing this group's admission or reserving a slot.
        // Retry submit after completion; another producer may acquire capacity first. Group/Runtime stop
        // completes with set_stopped. Use unmonitored execution.spawn: waiting must not consume a task slot.
        // The connected operation borrows ExecutionRuntime through completion, like accepted work.
        // whenSettled() does not join this unmonitored pipeline or its downstream continuations;
        // the enclosing operation must keep its consumer alive through their terminal delivery.
        [[nodiscard]] CapacitySender whenCapacityAvailable() noexcept;
        // Explicit synchronous barrier: closes admission and collects accepted work to completion.
        // Only shutdown, synchronous startup/APIs and tests may wait; object/project destruction must not.
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> join() noexcept;

    private:
        ExecutionRuntime& execution_;
        std::shared_ptr<detail::TaskGroup> group_;
    };

    class TaskScope::CompletionSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

        [[nodiscard]] stdexec::env<> get_env() const noexcept
        {
            return {};
        }

        template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
        {
            auto sender = stdexec::let_value(
                stdexec::just(group_),
                [tasks = tasks_](std::shared_ptr<detail::TaskGroup>& group) noexcept
                {
                    detail::closeTaskGroup(*tasks, group);
                    return stdexec::write_env(
                        stdexec::then(
                            group->scope.join(),
                            [group]() noexcept { group->joined.store(true, std::memory_order_release); }
                        ),
                        stdexec::prop{stdexec::get_scheduler, stdexec::inline_scheduler{}}
                    );
                }
            );
            return stdexec::connect(std::move(sender), std::move(receiver));
        }

    private:
        friend class TaskScope;

        CompletionSender(detail::TaskRuntime& tasks, std::shared_ptr<detail::TaskGroup> group) noexcept
            : tasks_(&tasks), group_(std::move(group))
        {
        }

        detail::TaskRuntime* tasks_;
        std::shared_ptr<detail::TaskGroup> group_;
    };

    class TaskScope::CapacitySender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>;

        [[nodiscard]] stdexec::env<> get_env() const noexcept
        {
            return {};
        }

        template <class Receiver> class Operation final : private detail::TaskCapacityWaiter
        {
        public:
            Operation(
                detail::TaskRuntime& tasks,
                std::shared_ptr<detail::TaskGroup> task_group,
                Receiver receiver
            ) noexcept
                : tasks_(tasks), receiver_(std::move(receiver))
            {
                group = std::move(task_group);
                code_lifetime = detail::getCompletionCode(stdexec::get_env(receiver_));
                complete = [](detail::TaskCapacityWaiter* waiter, bool stopped) noexcept
                {
                    auto& operation = *static_cast<Operation*>(waiter);
                    if (stopped)
                    {
                        stdexec::set_stopped(std::move(operation.receiver_));
                    }
                    else
                    {
                        stdexec::set_value(std::move(operation.receiver_));
                    }
                };
            }

            void start() & noexcept
            {
                detail::waitForTaskCapacity(tasks_, *this);
            }

        private:
            detail::TaskRuntime& tasks_;
            Receiver receiver_;
        };

        template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
        {
            return Operation<Receiver>{*tasks_, group_, std::move(receiver)};
        }

    private:
        friend class TaskScope;

        CapacitySender(detail::TaskRuntime& tasks, std::shared_ptr<detail::TaskGroup> group) noexcept
            : tasks_(&tasks), group_(std::move(group))
        {
        }

        detail::TaskRuntime* tasks_;
        std::shared_ptr<detail::TaskGroup> group_;
    };
} // namespace lux::execution
