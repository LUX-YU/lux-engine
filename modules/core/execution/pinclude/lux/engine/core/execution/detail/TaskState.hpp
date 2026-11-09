#pragma once

#include <lux/engine/core/execution/ExecutionRuntime.hpp>

#include <mutex>
#include <lux/engine/core/execution/detail/TaskGroup.hpp>

namespace lux::execution::detail
{
    struct TaskCapacityWaiter;

    struct TaskRecord final
    {
        TaskRuntime* runtime;
        TaskInfo info;
        std::stop_source stop;
        // Covers sender state destruction. External producers retain their own callback code pin.
        std::shared_ptr<const void> code_lifetime;
        std::shared_ptr<TaskGroup> group;
        TaskGroupAssociation group_association;
        TaskRecord* group_next{};
        TaskRecord** group_previous{};
        // One submission-stack reference and one stdexec allocation reference.
        std::atomic<unsigned> accesses{2};
        bool dirty{};
    };

    struct TaskRuntime final
    {
        TaskRuntime(ExecutionRuntime&, std::size_t capacity, std::size_t history_capacity);
        ~TaskRuntime();
        void requireMainThread() const noexcept;
        void changed(TaskRecord&) noexcept; // Caller holds mutex.
        [[nodiscard]] TaskCapacityWaiter* takeCapacityWaiters(const TaskGroup* group = nullptr) noexcept;
        [[nodiscard]] TaskRecord* find(TaskId) const noexcept; // Caller holds mutex.
        void retire(TaskRecord&) noexcept;
        void stop() noexcept;
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] bool hasWork() const noexcept;
        [[nodiscard]] std::size_t dispatch() noexcept;
        [[nodiscard]] bool requestStop(TaskId) noexcept;
        [[nodiscard]] std::optional<TaskInfo> info(TaskId) const noexcept;
        [[nodiscard]] std::vector<TaskInfo> infos() const noexcept;

        ExecutionRuntime* execution;
        ExecutionState& execution_state;
        const std::thread::id main_thread{std::this_thread::get_id()};
        const std::uint64_t identity;
        const std::size_t capacity;
        const std::size_t history_capacity;
        mutable std::mutex mutex;
        lux::cxx::SlotMap<std::unique_ptr<TaskRecord>, TaskTag> records;
        std::vector<TaskId> changes;
        std::vector<TaskId> change_batch;
        std::vector<TaskInfo> history;
        std::size_t history_next{};
        bool accepting{true};
        bool dispatching{};
        bool resync{};
        void* observer_context{};
        ExecutionRuntime::TaskObserver observer{};
        TaskCapacityWaiter* capacity_waiters{};
    };

    // Detached before invocation; receivers may destroy their connected operation.
    void completeCapacityWaiters(TaskCapacityWaiter*, bool stopped) noexcept;
} // namespace lux::execution::detail
