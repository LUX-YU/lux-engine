#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/core/execution/detail/TaskState.hpp>

#include <algorithm>
#include <atomic>

namespace lux::execution::detail
{
    namespace
    {
        std::atomic<std::uint64_t> next_runtime{1};
    }

    TaskRuntime::TaskRuntime(ExecutionRuntime& execution, std::size_t limit, std::size_t history_limit)
        : execution(&execution), identity(next_runtime.fetch_add(1, std::memory_order_relaxed)), capacity(limit),
          history_capacity(history_limit)
    {
        if (identity == 0)
        {
            std::terminate();
        }
        records.reserve(capacity);
        changes.reserve(capacity + history_capacity);
        change_batch.reserve(capacity + history_capacity);
        history.reserve(history_capacity);
    }

    TaskRuntime::~TaskRuntime()
    {
        // Task/TaskScope are lifetime borrows of their Runtime.
        if (!records.empty())
        {
            std::terminate();
        }
    }

    void TaskRuntime::requireMainThread() const noexcept
    {
        if (main_thread != std::this_thread::get_id())
        {
            std::terminate();
        }
    }

    void TaskRuntime::moveTo(ExecutionRuntime& execution) noexcept
    {
        std::lock_guard lock{mutex};
        if (!records.empty())
        {
            std::terminate();
        }
        this->execution = &execution;
    }

    TaskRecord* TaskRuntime::find(TaskId id) const noexcept
    {
        if (id.runtime != identity)
        {
            return nullptr;
        }
        const auto* value = records.find(id.slot);
        return value ? value->get() : nullptr;
    }

    void TaskRuntime::changed(TaskRecord& record) noexcept
    {
        if (std::exchange(record.dirty, true))
        {
            return;
        }
        if (changes.size() == changes.capacity())
        {
            // Monitoring can coalesce; business results are delivered by their sender pipelines.
            resync = true;
            return;
        }
        changes.push_back(record.info.id);
    }

    lux::cxx::expected<TaskRecord*, EExecutionError> admitTask(
        TaskRuntime& runtime,
        TaskOptions options,
        std::shared_ptr<TaskGroup> group
    ) noexcept
    {
        std::lock_guard lock{runtime.mutex};
        if (!runtime.accepting || (group && !group->accepting))
        {
            return lux::cxx::unexpected(EExecutionError::STOPPING);
        }
        if (runtime.records.size() == runtime.capacity)
        {
            return lux::cxx::unexpected(EExecutionError::CAPACITY_EXCEEDED);
        }
        auto record = std::make_unique<TaskRecord>();
        record->runtime = &runtime;
        record->info.name = std::move(options.name);
        record->info.category = std::move(options.category);
        record->info.parent = options.parent;
        record->info.submitted = std::chrono::steady_clock::now();
        record->info.state = ETaskState::RUNNING;
        record->info.started = record->info.submitted;
        record->code_lifetime = std::move(options.code_lifetime);
        record->group = group;
        auto* result = record.get();
        result->info.id = {runtime.identity, runtime.records.emplace(std::move(record))};
        if (group)
        {
            ++group->outstanding;
        }
        runtime.changed(*result);
        return result;
    }

    stdexec::simple_counting_scope& taskScope(TaskRecord& record) noexcept
    {
        return record.runtime->execution_scope;
    }

    void releaseTaskAccess(TaskRecord& record) noexcept
    {
        auto& execution = *record.runtime->execution;
        if (record.accesses.fetch_sub(1, std::memory_order_acq_rel) != 1)
        {
            // Admission itself changes monitoring. The wake may revoke the submitting scope;
            // no scope or task access follows it on this submission tail.
            execution.wake();
            return;
        }
        auto& runtime = *record.runtime;
        std::shared_ptr<const void> pin;
        {
            std::lock_guard lock{runtime.mutex};
            if (record.group)
            {
                --record.group->outstanding;
            }
            pin = std::move(record.code_lifetime);
            runtime.retire(record);
        }
        // Scope association is released by stdexec after its allocation/deallocation call.
        // Runtime join observes that independently; no TaskRecord access after retirement.
        runtime.execution->wake();
    }

    void completeTask(TaskRecord& record, ETaskState state) noexcept
    {
        auto& runtime = *record.runtime;
        {
            std::lock_guard lock{runtime.mutex};
            record.info.state = state;
            record.info.finished = std::chrono::steady_clock::now();
            runtime.changed(record);
        }
        // Notify monitoring without transferring the result to a Main-thread result queue.
        runtime.execution->wake();
    }

    TaskId taskId(const TaskRecord& record) noexcept
    {
        return record.info.id;
    }

    std::shared_ptr<const void> taskCodeLifetime(const TaskRecord& record) noexcept
    {
        return record.code_lifetime;
    }

    void TaskRuntime::retire(TaskRecord& record) noexcept
    {
        changed(record); // With history disabled this is also the removal notification.
        const auto slot = record.info.id.slot;
        if (history_capacity)
        {
            if (history.size() < history_capacity)
            {
                history.push_back(std::move(record.info));
            }
            else
            {
                history[history_next] = std::move(record.info);
                history_next = (history_next + 1) % history_capacity;
                resync = true;
            }
        }
        records.erase(slot);
    }

    std::size_t TaskRuntime::dispatch() noexcept
    {
        requireMainThread();
        if (dispatching)
        {
            std::terminate();
        }
        dispatching = true;
        bool refresh{};
        {
            std::lock_guard lock{mutex};
            changes.swap(change_batch);
            refresh = std::exchange(resync, false);
            if (refresh)
            {
                for (auto& record : records)
                {
                    record->dirty = false;
                }
            }
            else
            {
                for (auto id : change_batch)
                {
                    if (auto* record = find(id))
                    {
                        record->dirty = false;
                    }
                }
            }
        }
        if (observer && (refresh || !change_batch.empty()))
        {
            observer(observer_context, change_batch, refresh);
        }
        const auto count = change_batch.size();
        change_batch.clear();
        dispatching = false;
        return count;
    }

    bool TaskRuntime::requestStop(TaskId id) noexcept
    {
        std::optional<std::stop_source> stop;
        {
            std::lock_guard lock{mutex};
            auto* record = find(id);
            if (!record || record->info.finished)
            {
                return false;
            }
            record->info.cancel_requested = true;
            stop = record->stop;
            changed(*record);
        }
        stop->request_stop();
        execution->wake();
        return true;
    }

    void TaskRuntime::stop() noexcept
    {
        requireMainThread();
        std::vector<std::stop_source> stops;
        {
            std::lock_guard lock{mutex};
            accepting = false;
            stops.reserve(records.size());
            for (auto& record : records)
            {
                if (record->info.finished)
                {
                    continue;
                }
                record->info.cancel_requested = true;
                stops.push_back(record->stop);
                changed(*record);
            }
        }
        for (auto& stop : stops)
        {
            stop.request_stop();
        }
    }

    bool TaskRuntime::settled() const noexcept
    {
        std::lock_guard lock{mutex};
        return records.empty();
    }

    bool TaskRuntime::hasWork() const noexcept
    {
        std::lock_guard lock{mutex};
        return !changes.empty() || resync;
    }

    std::optional<TaskInfo> TaskRuntime::info(TaskId id) const noexcept
    {
        std::lock_guard lock{mutex};
        if (const auto* record = find(id))
        {
            return record->info;
        }
        const auto it = std::ranges::find(history, id, &TaskInfo::id);
        return it == history.end() ? std::nullopt : std::optional{*it};
    }

    std::vector<TaskInfo> TaskRuntime::infos() const noexcept
    {
        std::lock_guard lock{mutex};
        std::vector<TaskInfo> result = history;
        result.reserve(result.size() + records.size());
        for (const auto& record : records)
        {
            result.push_back(record->info);
        }
        return result;
    }
} // namespace lux::execution::detail

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
        auto& tasks = *execution_.tasks_;
        std::vector<std::stop_source> stops;
        {
            std::lock_guard lock{tasks.mutex};
            group_->accepting = false;
            if (group_->outstanding == 0)
            {
                return;
            }
            stops.reserve(group_->outstanding);
            for (auto& record : tasks.records)
            {
                if (record->group != group_ || record->info.finished)
                {
                    continue;
                }
                record->info.cancel_requested = true;
                stops.push_back(record->stop);
                tasks.changed(*record);
            }
        }
        for (auto& stop : stops)
        {
            stop.request_stop();
        }
        execution_.wake();
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

    Task::Task(Task&& other) noexcept : tasks_(std::exchange(other.tasks_, nullptr)), id_(other.id_) {}

    Task& Task::operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            requestStop();
            tasks_ = std::exchange(other.tasks_, nullptr);
            id_ = other.id_;
        }
        return *this;
    }

    Task::~Task() noexcept
    {
        requestStop();
    }

    TaskId Task::id() const noexcept
    {
        return tasks_ ? id_ : TaskId{};
    }

    void Task::requestStop() noexcept
    {
        if (tasks_)
        {
            static_cast<void>(tasks_->requestStop(id_));
        }
    }

    TaskId TaskReporter::id() const noexcept
    {
        return record_ ? record_->info.id : TaskId{};
    }

    std::stop_token TaskReporter::stopToken() const noexcept
    {
        return record_ ? record_->stop.get_token() : std::stop_token{};
    }

    void TaskReporter::setPhase(std::string_view phase) const noexcept
    {
        if (!record_)
        {
            return;
        }
        auto& runtime = *record_->runtime;
        {
            std::lock_guard lock{runtime.mutex};
            record_->info.phase = phase;
            runtime.changed(*record_);
        }
        runtime.execution->wake();
    }

    void TaskReporter::setProgress(std::uint64_t completed, std::uint64_t total) const noexcept
    {
        if (!record_)
        {
            return;
        }
        auto& runtime = *record_->runtime;
        {
            std::lock_guard lock{runtime.mutex};
            record_->info.progress = TaskProgress{completed, total};
            runtime.changed(*record_);
        }
        runtime.execution->wake();
    }
} // namespace lux::execution
