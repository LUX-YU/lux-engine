#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/core/execution/detail/TaskState.hpp>

namespace lux::execution
{
    void detail::waitForTaskCapacity(TaskRuntime& tasks, TaskCapacityWaiter& waiter) noexcept
    {
        bool stopped{};
        {
            std::lock_guard lock{tasks.mutex};
            stopped = !tasks.accepting || !waiter.group->accepting;
            if (!stopped && tasks.records.size() == tasks.capacity)
            {
                waiter.next = std::exchange(tasks.capacity_waiters, &waiter);
                return;
            }
        }
        auto code_lifetime = std::move(waiter.code_lifetime);
        waiter.complete(&waiter, stopped);
    }

    void detail::closeTaskGroup(
        TaskRuntime& tasks,
        const std::shared_ptr<TaskGroup>& group,
        TaskGroupWaiter& waiter
    ) noexcept
    {
        TaskCapacityWaiter* capacity_waiters{};
        bool settled{};
        {
            std::lock_guard lock{tasks.mutex};
            group->accepting = false;
            capacity_waiters = tasks.takeCapacityWaiters(group.get());
            settled = group->outstanding == 0;
            if (!settled)
            {
                waiter.next = std::exchange(group->waiters, &waiter);
            }
        }
        completeCapacityWaiters(capacity_waiters, true);
        if (settled)
        {
            auto code_lifetime = std::move(waiter.code_lifetime);
            waiter.complete(&waiter);
        }
    }

    TaskScope::TaskScope(ExecutionRuntime& execution) noexcept
        : execution_(execution), group_(std::make_shared<detail::TaskGroup>())
    {
    }

    TaskScope::~TaskScope() noexcept
    {
        requestStop();
    }

    void TaskScope::requestStop() noexcept
    {
        auto& execution = execution_;
        auto& tasks = *execution.tasks_;
        auto group = group_;
        std::vector<std::stop_source> stops;
        detail::TaskCapacityWaiter* capacity_waiters{};
        {
            std::lock_guard lock{tasks.mutex};
            group->accepting = false;
            capacity_waiters = tasks.takeCapacityWaiters(group.get());
            stops.reserve(group->outstanding);
            for (auto& record : tasks.records)
            {
                if (record->group != group || record->info.finished)
                {
                    continue;
                }
                record->info.cancel_requested = true;
                stops.push_back(record->stop);
                tasks.changed(*record);
            }
        }
        group->stop.request_stop();
        for (auto& stop : stops)
        {
            stop.request_stop();
        }
        detail::completeCapacityWaiters(capacity_waiters, true);
        execution.wake();
    }

    std::stop_token TaskScope::stopToken() const noexcept
    {
        return group_->stop.get_token();
    }

    bool TaskScope::settled() const noexcept
    {
        auto& tasks = *execution_.tasks_;
        std::lock_guard lock{tasks.mutex};
        return group_->outstanding == 0;
    }

    TaskScope::CompletionSender TaskScope::whenSettled() noexcept
    {
        return CompletionSender{*execution_.tasks_, group_};
    }

    TaskScope::CapacitySender TaskScope::whenCapacityAvailable() noexcept
    {
        return CapacitySender{*execution_.tasks_, group_};
    }

    lux::cxx::expected<void, EExecutionError> TaskScope::join() noexcept
    {
        auto& execution = execution_;
        auto& tasks = *execution.tasks_;
        auto group = group_;
        if (tasks.main_thread != std::this_thread::get_id())
        {
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        }
        detail::TaskCapacityWaiter* capacity_waiters{};
        bool settled{};
        {
            std::lock_guard lock{tasks.mutex};
            group->accepting = false;
            capacity_waiters = tasks.takeCapacityWaiters(group.get());
            settled = group->outstanding == 0;
        }
        detail::completeCapacityWaiters(capacity_waiters, true);
        if (settled)
        {
            return {};
        }
        const auto valid = execution.validateWait();
        if (!valid)
        {
            return lux::cxx::unexpected(valid.error());
        }
        for (;;)
        {
            const auto epoch = execution.wakeEpoch();
            if (!execution.dispatchMainCompletions())
            {
                std::terminate();
            }
            {
                std::lock_guard lock{tasks.mutex};
                if (group->outstanding == 0)
                {
                    return {};
                }
            }
            execution.waitForWork(epoch);
        }
    }

} // namespace lux::execution
