#pragma once

#include <lux/engine/core/execution/ExecutionRuntime.hpp>

namespace lux::execution
{
    namespace detail
    {
        struct TaskGroupWaiter
        {
            TaskGroupWaiter* next{};
            std::shared_ptr<const void> code_lifetime;
            void (*complete)(TaskGroupWaiter*) noexcept {};
        };

        LUX_CORE_EXECUTION_PUBLIC void
        closeTaskGroup(TaskRuntime&, const std::shared_ptr<TaskGroup>&, TaskGroupWaiter&) noexcept;
    } // namespace detail

    class LUX_CORE_EXECUTION_PUBLIC TaskScope final
    {
    public:
        class CompletionSender;
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

        // Accepted finite work outlives this cancellation group. Its sender explicitly chooses
        // transport/adoption threads and consumes business values before completing.
        template <class Factory>
        [[nodiscard]] lux::cxx::expected<TaskId, EExecutionError> submit(
            TaskOptions options,
            Factory&& make_sender
        ) noexcept
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
        // Such children are joined by ExecutionRuntime or their enclosing monitored operation.
        [[nodiscard]] std::stop_token stopToken() const noexcept;
        // Thread-safe observation only: no waiting, callbacks or admission changes.
        [[nodiscard]] bool settled() const noexcept;
        // Close admission when started; complete after the accepted operation states have ended.
        // No cancellation or nested loop. The waiting operation must be outside this group.
        [[nodiscard]] CompletionSender whenSettled() noexcept;
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

        template <class Receiver> class Operation final : private detail::TaskGroupWaiter
        {
        public:
            Operation(detail::TaskRuntime& tasks, std::shared_ptr<detail::TaskGroup> group, Receiver receiver) noexcept
                : tasks_(tasks), group_(std::move(group)), receiver_(std::move(receiver))
            {
                code_lifetime = detail::getCompletionCode(stdexec::get_env(receiver_));
                complete = [](detail::TaskGroupWaiter* waiter) noexcept
                {
                    auto& operation = *static_cast<Operation*>(waiter);
                    stdexec::set_value(std::move(operation.receiver_));
                };
            }

            void start() & noexcept
            {
                detail::closeTaskGroup(tasks_, group_, *this);
            }

        private:
            detail::TaskRuntime& tasks_;
            std::shared_ptr<detail::TaskGroup> group_;
            Receiver receiver_;
        };

        template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
        {
            return Operation<Receiver>{*tasks_, group_, std::move(receiver)};
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
} // namespace lux::execution
