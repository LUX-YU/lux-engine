#include <lux/engine/editor/ui/TaskPane.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
namespace lux::editor::ui
{
    // Old menu/rooted construction only, P12. Task display and cancellation use the shared implementation.
    struct TaskPane::Impl final
    {
        tasks::TaskListElement content_;
        object::Connection close_;
        bool hide_{};
        Impl(TaskPane& pane, EditorContext& context, EditorResult<void>& status) : content_(pane, context.taskMonitor())
        {
            pane.setContent(content_);
            close_ = detail::takeConnection(
                object::LuxObject::connect(&pane, &lux::ui::Pane::closeRequested, [this]() noexcept { hide_ = true; }),
                status
            );
        }
    };
    TaskPane::TaskPane(lux::ui::Root& root, EditorContext& context, EditorResult<void>& status)
        : Pane(root, lux::ui::PaneId{"tasks"}, lux::ui::PaneTypeId{"lux.editor.tasks"}, "Background tasks"),
          impl_(std::make_unique<Impl>(*this, context, status))
    {}
    TaskPane::~TaskPane() noexcept = default;
    void TaskPane::update() noexcept
    {
        if (std::exchange(impl_->hide_, false))
            setVisible(false);
    }
}
