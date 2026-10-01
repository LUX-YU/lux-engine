#pragma once
#include <lux/engine/editor/tasks/TaskMonitor.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>
#include <lux/engine/ui/Element.hpp>
namespace lux::editor::tasks
{
    // Shared table implementation, also used by the old product adapter until P12.
    class TaskListElement final : public lux::ui::Element
    {
    public:
        TaskListElement(lux::ui::Pane&, TaskMonitor&);
        TaskListElement(const TaskListElement&) = delete;
        TaskListElement& operator=(const TaskListElement&) = delete;
        TaskListElement(TaskListElement&&) = delete;
        TaskListElement& operator=(TaskListElement&&) = delete;
        [[nodiscard]] std::span<const process::TaskInfo> rows() const noexcept
        {
            return *rows_;
        }
        void requestCancel(process::TaskId);
        [[nodiscard]] std::span<const process::TaskId> rejectedCancellations() const noexcept
        {
            return rejected_;
        }

    private:
        void update() noexcept override;
        void draw() noexcept override;
        TaskMonitor& query_;
        TaskMonitor::Snapshot rows_;
        std::vector<process::TaskId> cancel_, rejected_;
        std::optional<std::uint64_t> revision_;
        object::Connection changes_;
    };
    class TaskView final : public lux::ui::Pane
    {
    public:
        TaskView(object::ObjectDispatcherRef, lux::ui::PaneId, TaskMonitor&);
        TaskView(const TaskView&) = delete;
        TaskView& operator=(const TaskView&) = delete;
        TaskView(TaskView&&) = delete;
        TaskView& operator=(TaskView&&) = delete;
        [[nodiscard]] TaskListElement& tasks() noexcept
        {
            return content_;
        }

    private:
        TaskListElement content_;
    };
    [[nodiscard]] views::DetachedView makeTaskView(object::ObjectDispatcherRef, lux::ui::PaneId, TaskMonitor&);
}
