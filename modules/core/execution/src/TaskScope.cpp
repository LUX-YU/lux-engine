#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/core/execution/detail/TaskState.hpp>

namespace lux::execution
{
    void detail::waitForTaskCapacity(TaskRuntime& tasks, TaskCapacityWaiter& waiter) noexcept
    {
        bool stopped{};
        {
            std::lock_guard lock{tasks.mutex};
            stopped = !tasks.accepting || !waiter.group->accepting.load(std::memory_order_acquire);
            if (!stopped && tasks.records.size() == tasks.capacity)
            {
                waiter.next = std::exchange(tasks.capacity_waiters, &waiter);
                waiter.previous = &tasks.capacity_waiters;
                if (waiter.next)
                {
                    waiter.next->previous = &waiter.next;
                }
                waiter.group_next = std::exchange(waiter.group->capacity_waiters, &waiter);
                waiter.group_previous = &waiter.group->capacity_waiters;
                if (waiter.group_next)
                {
                    waiter.group_next->group_previous = &waiter.group_next;
                }
                return;
            }
        }
        auto code_lifetime = std::move(waiter.code_lifetime);
        waiter.complete(&waiter, stopped);
    }

    void detail::closeTaskGroup(TaskRuntime& tasks, const std::shared_ptr<TaskGroup>& group) noexcept
    {
        TaskCapacityWaiter* capacity_waiters{};
        {
            std::lock_guard lock{tasks.mutex};
            group->accepting.store(false, std::memory_order_release);
            capacity_waiters = tasks.takeCapacityWaiters(group.get());
        }
        completeCapacityWaiters(capacity_waiters, true);
        group->close();
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
            if (std::exchange(group->stop_requested, true))
            {
                return;
            }
            group->accepting.store(false, std::memory_order_release);
            capacity_waiters = tasks.takeCapacityWaiters(group.get());
            stops.reserve(group->task_count);
            for (auto* record = group->first_task; record; record = record->group_next)
            {
                if (record->info.finished)
                {
                    continue;
                }
                record->info.cancel_requested = true;
                stops.push_back(record->stop);
                tasks.changed(*record);
            }
        }
        group->close();
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
        return group_->active_operations.load(std::memory_order_acquire) == 0;
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
        detail::closeTaskGroup(tasks, group);
        return execution.waitUntil([&group]() noexcept { return group->joined.load(std::memory_order_acquire); });
    }

} // namespace lux::execution
