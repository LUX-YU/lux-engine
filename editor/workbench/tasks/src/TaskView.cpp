#include <exception>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
#include <imgui.h>
#include <algorithm>
namespace lux::editor::tasks
{
    namespace
    {
        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.tasks"},
            "Background Tasks",
            "Window"
        };
        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{
            views::ViewTypeIdView{"lux.editor.tasks"},
            "Tasks",
            cxx::typeToken<std::monostate>()
        };
    } // namespace
    TaskListElement::TaskListElement(lux::ui::Pane& parent, TaskMonitor& query)
        : Element(parent, lux::ui::ElementId{"tasks"}), query_(query), rows_(query_.snapshot()),
          revision_(query.revision())
    {
        auto connected = object::LuxObject::connect(
            &query_,
            &TaskMonitor::changed,
            [this](std::uint64_t) noexcept { revision_.reset(); }
        );
        if (!connected)
            std::terminate();
        changes_ = std::move(*connected);
    }
    void TaskListElement::requestCancel(process::TaskId id)
    {
        if (std::ranges::find(cancel_, id) == cancel_.end())
            cancel_.push_back(id);
    }
    void TaskListElement::update() noexcept
    {
        rejected_.clear();
        for (auto id : cancel_)
            if (!query_.requestCancel(id))
                rejected_.push_back(id);
        cancel_.clear();
        const auto revision = query_.revision();
        if (revision != revision_)
        {
            rows_ = query_.snapshot();
            revision_ = revision;
        }
    }
    TaskView::TaskView(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, TaskMonitor& query)
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{kFactoryDescriptor.type.name()}, "Background tasks"),
          content_(*this, query)
    {
        if (!setContent(content_))
            std::terminate(); // Fixed content in a detached Pane.
    }
    views::DetachedView makeTaskView(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, TaskMonitor& query)
    {
        return {lux::object::CodeLease::builtin(), std::make_unique<TaskView>(dispatcher, std::move(id), query)};
    }
    void TaskListElement::draw() noexcept
    {
        constexpr const char* states[]{"Queued", "Running", "Succeeded", "Failed", "Cancelled"};
        if (!ImGui::BeginTable("tasks", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
            return;
        ImGui::TableSetupColumn("Task");
        ImGui::TableSetupColumn("Stage");
        ImGui::TableSetupColumn("Progress");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_->size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto& task = (*rows_)[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(task.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(task.phase.c_str());
                ImGui::TableNextColumn();
                if (task.progress && task.progress->total != 0)
                    ImGui::Text(
                        "%.0f%%",
                        100.0 * static_cast<double>(task.progress->completed) /
                            static_cast<double>(task.progress->total)
                    );
                else
                    ImGui::TextUnformatted("--");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(states[static_cast<unsigned>(task.state)]);
                ImGui::TableNextColumn();
                if (!task.finished)
                {
                    ImGui::BeginDisabled(task.cancel_requested);
                    if (ImGui::SmallButton("Cancel"))
                        cancel_.push_back(task.id);
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
} // namespace lux::editor::tasks

namespace lux::editor::tasks
{
    std::shared_ptr<views::ViewFactoryEntry> makeTaskViewFactory(TaskMonitor& monitor)
    {
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(
            lux::object::CodeLease::builtin(),
            [&monitor](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView>
            { return makeTaskView(input.dispatcher(), input.paneId(), monitor); }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeTasksCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kCommand, kFactoryDescriptor>(std::move(query), std::move(open));
    }

} // namespace lux::editor::tasks
