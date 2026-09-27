#pragma once

#include <lux/engine/process/ExecutionRuntime.hpp>

#include <mutex>

namespace lux::process::detail
{
    struct TaskGroup final
    {
        bool accepting{true};
        std::size_t outstanding{};
    };

    struct TaskRecord final
    {
        TaskRuntime* runtime;
        TaskInfo info;
        std::stop_source stop;
        // The pin must outlive the operation's virtual destructor, including plugin code.
        std::shared_ptr<const void> code_lifetime;
        std::unique_ptr<TaskOperation> operation;
        TaskGroup* group{};
        bool starting{true};
        bool collected{};
        bool dirty{};
    };

    struct TaskRuntime final
    {
        TaskRuntime(ExecutionRuntime&, std::size_t capacity, std::size_t history_capacity);
        ~TaskRuntime();
        void moveTo(ExecutionRuntime&) noexcept;
        void requireOwner() const noexcept;
        void changed(TaskRecord&) noexcept;                    // Caller holds mutex.
        [[nodiscard]] TaskRecord* find(TaskId) const noexcept; // Caller holds mutex.
        void retire(TaskRecord&) noexcept;
        void release(TaskRecord&) noexcept;
        void stop() noexcept;
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] bool hasWork() const noexcept;
        [[nodiscard]] std::size_t collect() noexcept;
        [[nodiscard]] std::size_t dispatch() noexcept;
        [[nodiscard]] bool requestStop(TaskId) noexcept;
        [[nodiscard]] std::optional<TaskInfo> info(TaskId) const noexcept;
        [[nodiscard]] std::vector<TaskInfo> infos() const noexcept;

        ExecutionRuntime* owner;
        const std::thread::id owner_thread{std::this_thread::get_id()};
        const std::uint64_t identity;
        const std::size_t capacity;
        const std::size_t history_capacity;
        mutable std::mutex mutex;
        lux::cxx::SlotMap<std::unique_ptr<TaskRecord>, TaskTag> records;
        std::vector<TaskRecord*> ready;
        std::vector<TaskRecord*> collect_batch;
        std::vector<TaskId> deliveries;
        std::vector<TaskId> delivery_batch;
        std::vector<TaskId> changes;
        std::vector<TaskId> change_batch;
        std::vector<TaskInfo> history;
        std::size_t history_next{};
        std::size_t outstanding{};
        bool accepting{true};
        bool collecting{};
        bool dispatching{};
        bool resync{};
        void* observer_owner{};
        ExecutionRuntime::TaskObserver observer{};
    };
}
