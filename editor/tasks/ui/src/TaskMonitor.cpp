#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/editor/tasks/TaskMonitor.hpp>

namespace lux::editor::tasks
{
    TaskMonitor::TaskMonitor(object::ObjectDispatcherRef dispatcher, process::ExecutionRuntime& runtime)
        : LuxObject(std::move(dispatcher)), runtime_(runtime)
    {
        runtime_.setTaskObserver(this, [](void* owner, std::span<const process::TaskId> ids, bool resync) noexcept {
            auto& monitor = *static_cast<TaskMonitor*>(owner);
            if (!ids.empty() || resync)
            {
                if (monitor.revision_ == UINT64_MAX)
                    std::terminate();
                ++monitor.revision_;
                monitor.pending_ = true;
            }
        });
    }
    TaskMonitor::~TaskMonitor()
    {
        runtime_.setTaskObserver(nullptr, nullptr);
    }
    TaskMonitor::Snapshot TaskMonitor::snapshot() const
    {
        if (captured_revision_ != revision_)
        {
            snapshot_ = std::make_shared<const std::vector<process::TaskInfo>>(runtime_.taskInfos());
            captured_revision_ = revision_;
        }
        return snapshot_;
    }
    bool TaskMonitor::requestCancel(process::TaskId id) noexcept
    {
        return runtime_.requestStop(id);
    }
    object::SignalDelivery TaskMonitor::dispatchChanges() noexcept
    {
        if (!std::exchange(pending_, false))
            return {};
        return emit(changed, revision_);
    }
}
