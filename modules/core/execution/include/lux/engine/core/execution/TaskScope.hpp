#pragma once

#include <lux/engine/core/execution/ExecutionRuntime.hpp>

namespace lux::execution
{
    class LUX_CORE_EXECUTION_PUBLIC TaskScope final
    {
    public:
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
        // Explicit synchronous barrier: closes admission and collects accepted work to completion.
        // Only shutdown, synchronous startup/APIs and tests may wait; object/project destruction must not.
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> join() noexcept;

    private:
        ExecutionRuntime& execution_;
        std::shared_ptr<detail::TaskGroup> group_;
    };
} // namespace lux::execution
