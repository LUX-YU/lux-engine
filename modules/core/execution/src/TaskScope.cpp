#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/core/execution/detail/TaskState.hpp>

namespace lux::execution
{
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
        {
            std::lock_guard lock{tasks.mutex};
            group->accepting = false;
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

    lux::cxx::expected<void, EExecutionError> TaskScope::join() noexcept
    {
        auto& tasks = *execution_.tasks_;
        if (tasks.main_thread != std::this_thread::get_id())
        {
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        }
        {
            std::lock_guard lock{tasks.mutex};
            group_->accepting = false;
            if (group_->outstanding == 0)
            {
                return {};
            }
        }
        const auto valid = execution_.validateWait();
        if (!valid)
        {
            return lux::cxx::unexpected(valid.error());
        }
        for (;;)
        {
            const auto epoch = execution_.wakeEpoch();
            if (!execution_.dispatchMainCompletions())
            {
                std::terminate();
            }
            {
                std::lock_guard lock{tasks.mutex};
                if (group_->outstanding == 0)
                {
                    return {};
                }
            }
            execution_.waitForWork(epoch);
        }
    }

} // namespace lux::execution
