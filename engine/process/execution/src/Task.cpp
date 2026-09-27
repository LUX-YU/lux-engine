#include <lux/engine/process/detail/TaskState.hpp>
#include <lux/engine/process/TaskScope.hpp>

#include <algorithm>
#include <atomic>

namespace lux::process::detail
{
    namespace
    {
        std::atomic<std::uint64_t> next_runtime{1};
    }

    TaskRuntime::TaskRuntime(ExecutionRuntime& execution, std::size_t limit, std::size_t history_limit)
        : owner(&execution), identity(next_runtime.fetch_add(1, std::memory_order_relaxed)), capacity(limit),
          history_capacity(history_limit)
    {
        if (identity == 0)
            std::terminate();
        records.reserve(capacity);
        ready.reserve(capacity);
        collect_batch.reserve(capacity);
        deliveries.reserve(capacity);
        delivery_batch.reserve(capacity);
        changes.reserve(capacity + history_capacity);
        change_batch.reserve(capacity + history_capacity);
        history.reserve(history_capacity);
    }

    TaskRuntime::~TaskRuntime()
    {
        // Task/TaskScope are lifetime borrows of their Runtime.
        if (!records.empty())
            std::terminate();
    }

    void TaskRuntime::requireOwner() const noexcept
    {
        if (owner_thread != std::this_thread::get_id())
            std::terminate();
    }

    void TaskRuntime::moveTo(ExecutionRuntime& execution) noexcept
    {
        std::lock_guard lock{mutex};
        if (!records.empty())
            std::terminate();
        owner = &execution;
    }

    TaskRecord* TaskRuntime::find(TaskId id) const noexcept
    {
        if (id.runtime != identity)
            return nullptr;
        const auto* value = records.find(id.slot);
        return value ? value->get() : nullptr;
    }

    void TaskRuntime::changed(TaskRecord& record) noexcept
    {
        if (std::exchange(record.dirty, true))
            return;
        if (changes.size() == changes.capacity())
        {
            // Monitoring can coalesce to a refresh; terminal delivery has a separate reliable queue.
            resync = true;
            return;
        }
        changes.push_back(record.info.id);
    }

    lux::cxx::expected<TaskRecord*, EExecutionError> admitTask(
        TaskRuntime& runtime,
        TaskOptions options,
        TaskGroup* group
    ) noexcept
    {
        std::lock_guard lock{runtime.mutex};
        if (!runtime.accepting || (group && !group->accepting))
            return lux::cxx::unexpected(EExecutionError::STOPPING);
        if (runtime.records.size() == runtime.capacity)
            return lux::cxx::unexpected(EExecutionError::CAPACITY_EXCEEDED);
        auto record = std::make_unique<TaskRecord>();
        record->runtime = &runtime;
        record->info.name = std::move(options.name);
        record->info.category = std::move(options.category);
        record->info.parent = options.parent;
        record->info.submitted = std::chrono::steady_clock::now();
        record->code_lifetime = std::move(options.code_lifetime);
        record->group = group;
        auto* result = record.get();
        result->info.id = {runtime.identity, runtime.records.emplace(std::move(record))};
        ++runtime.outstanding;
        if (group)
            ++group->outstanding;
        runtime.changed(*result);
        return result;
    }

    void startTask(TaskRecord& record, std::unique_ptr<TaskOperation> operation) noexcept
    {
        auto& runtime = *record.runtime;
        {
            std::lock_guard lock{runtime.mutex};
            record.operation = std::move(operation);
            record.info.state = ETaskState::RUNNING;
            record.info.started = std::chrono::steady_clock::now();
            runtime.changed(record);
        }
        record.operation->start();
        {
            std::lock_guard lock{runtime.mutex};
            record.starting = false;
        }
        runtime.owner->wake();
    }

    void completeTask(TaskRecord& record, ETaskState state) noexcept
    {
        auto& runtime = *record.runtime;
        {
            std::lock_guard lock{runtime.mutex};
            record.info.state = state;
            record.info.finished = std::chrono::steady_clock::now();
            runtime.ready.push_back(&record);
            runtime.changed(record);
        }
        // No operation/record access after publication: an owner may now collect it.
        runtime.owner->wake();
    }

    TaskId taskId(const TaskRecord& record) noexcept
    {
        return record.info.id;
    }

    std::size_t TaskRuntime::collect() noexcept
    {
        requireOwner();
        if (collecting)
            std::terminate();
        collecting = true;
        {
            std::lock_guard lock{mutex};
            ready.swap(collect_batch);
        }
        std::size_t count{};
        for (auto* record : collect_batch)
        {
            {
                std::lock_guard lock{mutex};
                if (record->starting)
                {
                    ready.push_back(record);
                    continue;
                }
                record->collected = true;
                --outstanding;
                if (!record->group)
                    deliveries.push_back(record->info.id);
            }
            if (record->group)
            {
                record->operation->deliver();
                record->operation.reset();
                record->code_lifetime.reset();
                std::lock_guard lock{mutex};
                --record->group->outstanding;
                retire(*record);
            }
            ++count;
        }
        collect_batch.clear();
        collecting = false;
        return count;
    }

    void TaskRuntime::retire(TaskRecord& record) noexcept
    {
        changed(record); // With history disabled this is also the removal notification.
        const auto slot = record.info.id.slot;
        if (history_capacity)
        {
            if (history.size() < history_capacity)
                history.push_back(std::move(record.info));
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
        requireOwner();
        if (dispatching || collecting)
            std::terminate();
        dispatching = true;
        bool refresh{};
        {
            std::lock_guard lock{mutex};
            deliveries.swap(delivery_batch);
            changes.swap(change_batch);
            refresh = std::exchange(resync, false);
            if (refresh)
            {
                for (auto& record : records)
                    record->dirty = false;
            }
            else
            {
                for (auto id : change_batch)
                    if (auto* record = find(id))
                        record->dirty = false;
            }
        }
        std::size_t delivered{};
        for (auto id : delivery_batch)
        {
            std::shared_ptr<const void> pin;
            std::unique_ptr<TaskOperation> operation;
            {
                std::lock_guard lock{mutex};
                if (auto* record = find(id))
                {
                    pin = std::move(record->code_lifetime);
                    operation = std::move(record->operation);
                }
            }
            if (!operation)
                continue; // The Task may have been destroyed by an earlier callback.
            operation->deliver();
            ++delivered;
        }
        if (observer && (refresh || !change_batch.empty()))
            observer(observer_owner, change_batch, refresh);
        delivery_batch.clear();
        change_batch.clear();
        dispatching = false;
        return delivered;
    }

    void TaskRuntime::release(TaskRecord& record) noexcept
    {
        requireOwner();
        const auto id = record.info.id;
        static_cast<void>(requestStop(id));
        while (!record.collected)
        {
            const auto epoch = owner->wakeEpoch();
            if (!owner->collectCompletions())
                std::terminate();
            if (!record.collected)
                owner->waitForWork(epoch);
        }
        record.operation.reset();
        record.code_lifetime.reset();
        std::lock_guard lock{mutex};
        // Remove undelivered callbacks immediately so rapid RAII tasks cannot fill the delivery buffer.
        std::erase(deliveries, id);
        retire(record);
    }

    bool TaskRuntime::requestStop(TaskId id) noexcept
    {
        std::optional<std::stop_source> stop;
        {
            std::lock_guard lock{mutex};
            auto* record = find(id);
            if (!record || record->info.finished)
                return false;
            record->info.cancel_requested = true;
            stop = record->stop;
            changed(*record);
        }
        stop->request_stop();
        owner->wake();
        return true;
    }

    void TaskRuntime::stop() noexcept
    {
        requireOwner();
        std::vector<std::stop_source> stops;
        {
            std::lock_guard lock{mutex};
            accepting = false;
            stops.reserve(outstanding);
            for (auto& record : records)
            {
                if (record->info.finished)
                    continue;
                record->info.cancel_requested = true;
                stops.push_back(record->stop);
                changed(*record);
            }
        }
        for (auto& stop : stops)
            stop.request_stop();
    }

    bool TaskRuntime::settled() const noexcept
    {
        std::lock_guard lock{mutex};
        return outstanding == 0;
    }

    bool TaskRuntime::hasWork() const noexcept
    {
        std::lock_guard lock{mutex};
        return !ready.empty() || !deliveries.empty() || !changes.empty() || resync;
    }

    std::optional<TaskInfo> TaskRuntime::info(TaskId id) const noexcept
    {
        std::lock_guard lock{mutex};
        if (const auto* record = find(id))
            return record->info;
        const auto it = std::ranges::find(history, id, &TaskInfo::id);
        return it == history.end() ? std::nullopt : std::optional{*it};
    }

    std::vector<TaskInfo> TaskRuntime::infos() const noexcept
    {
        std::lock_guard lock{mutex};
        std::vector<TaskInfo> result = history;
        result.reserve(result.size() + records.size());
        for (const auto& record : records)
            result.push_back(record->info);
        return result;
    }
}

namespace lux::process
{
    TaskScope::TaskScope(ExecutionRuntime& runtime) noexcept
        : runtime_(runtime), group_(std::make_unique<detail::TaskGroup>())
    {}

    TaskScope::~TaskScope() noexcept
    {
        requestStop();
        if (!join())
            std::terminate();
    }

    void TaskScope::requestStop() noexcept
    {
        auto& tasks = *runtime_.tasks_;
        std::vector<std::stop_source> stops;
        {
            std::lock_guard lock{tasks.mutex};
            group_->accepting = false;
            if (group_->outstanding == 0)
                return;
            stops.reserve(group_->outstanding);
            for (auto& record : tasks.records)
            {
                if (record->group != group_.get() || record->info.finished)
                    continue;
                record->info.cancel_requested = true;
                stops.push_back(record->stop);
                tasks.changed(*record);
            }
        }
        for (auto& stop : stops)
            stop.request_stop();
        runtime_.wake();
    }

    lux::cxx::expected<void, EExecutionError> TaskScope::join() noexcept
    {
        auto& tasks = *runtime_.tasks_;
        if (tasks.owner_thread != std::this_thread::get_id())
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        {
            std::lock_guard lock{tasks.mutex};
            group_->accepting = false;
            if (group_->outstanding == 0)
                return {};
        }
        const auto valid = runtime_.validateWait();
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        for (;;)
        {
            const auto epoch = runtime_.wakeEpoch();
            if (!runtime_.collectCompletions())
                std::terminate();
            {
                std::lock_guard lock{tasks.mutex};
                if (group_->outstanding == 0)
                    return {};
            }
            runtime_.waitForWork(epoch);
        }
    }

    Task::Task(Task&& other) noexcept : record_(std::exchange(other.record_, nullptr)) {}
    Task& Task::operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (record_)
                record_->runtime->release(*record_);
            record_ = std::exchange(other.record_, nullptr);
        }
        return *this;
    }
    Task::~Task() noexcept
    {
        if (record_)
            record_->runtime->release(*record_);
    }
    TaskId Task::id() const noexcept
    {
        return record_ ? record_->info.id : TaskId{};
    }
    void Task::requestStop() noexcept
    {
        if (record_)
            static_cast<void>(record_->runtime->requestStop(id()));
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
            return;
        auto& runtime = *record_->runtime;
        {
            std::lock_guard lock{runtime.mutex};
            record_->info.phase = phase;
            runtime.changed(*record_);
        }
        runtime.owner->wake();
    }
    void TaskReporter::setProgress(std::uint64_t completed, std::uint64_t total) const noexcept
    {
        if (!record_)
            return;
        auto& runtime = *record_->runtime;
        {
            std::lock_guard lock{runtime.mutex};
            record_->info.progress = TaskProgress{completed, total};
            runtime.changed(*record_);
        }
        runtime.owner->wake();
    }
}
