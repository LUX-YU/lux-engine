#pragma once
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/process/TaskInfo.hpp>
#include <memory>
#include <vector>

namespace lux::process
{
    class ExecutionRuntime;
}

namespace lux::editor::tasks
{
    // One application-owned observer per Runtime. Views only hold Connections and immutable projections.
    class TaskMonitor final : public object::LuxObject
    {
    public:
        using Snapshot = std::shared_ptr<const std::vector<process::TaskInfo>>;
        object::TSignal<std::uint64_t> changed{*this};
        TaskMonitor(object::ObjectDispatcherRef, process::ExecutionRuntime&);
        ~TaskMonitor() override;
        TaskMonitor(const TaskMonitor&) = delete;
        TaskMonitor& operator=(const TaskMonitor&) = delete;
        TaskMonitor(TaskMonitor&&) = delete;
        TaskMonitor& operator=(TaskMonitor&&) = delete;
        [[nodiscard]] std::uint64_t revision() const noexcept
        {
            return revision_;
        }
        [[nodiscard]] Snapshot snapshot() const;
        [[nodiscard]] bool requestCancel(process::TaskId) noexcept;
        // Call after ExecutionRuntime::dispatchTaskEvents has returned, never from the observer callback.
        [[nodiscard]] object::SignalDelivery dispatchChanges() noexcept;

    private:
        process::ExecutionRuntime& runtime_;
        std::uint64_t revision_{1};
        bool pending_{};
        mutable std::uint64_t captured_revision_{};
        mutable Snapshot snapshot_;
    };
}
