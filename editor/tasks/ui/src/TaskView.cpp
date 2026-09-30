#include <lux/engine/editor/tasks/TaskView.hpp>
#include <imgui.h>
#include <algorithm>
namespace lux::editor::tasks
{
    TaskListElement::TaskListElement(lux::ui::Pane& parent, TaskQueryPort query)
        : Element(parent, lux::ui::ElementId{"tasks"}), query_(query), rows_(query_.query())
    {}
    void TaskListElement::requestCancel(process::TaskId id)
    {
        if (std::ranges::find(cancel_, id) == cancel_.end())
            cancel_.push_back(id);
    }
    void TaskListElement::update() noexcept
    {
        rejected_.clear();
        for (auto id : cancel_)
            if (!query_.cancel(id))
                rejected_.push_back(id);
        cancel_.clear();
        // The application's existing Runtime observer can supply its revision. No second observer or
        // task catalog is registered here. Without that hint, conservatively requery the bounded catalog.
        const auto revision = query_.revision();
        if (!revision || revision != revision_)
        {
            rows_ = query_.query();
            revision_ = revision;
        }
    }
    TaskView::TaskView(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, TaskQueryPort query)
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.tasks"}, "Background tasks"),
          content_(*this, query)
    {
        setContent(content_);
    }
    views::DetachedView makeTaskView(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, TaskQueryPort query)
    {
        return {contracts::CodeLease::builtin(), std::make_unique<TaskView>(dispatcher, std::move(id), query)};
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
        clipper.Begin(static_cast<int>(rows_.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto& task = rows_[i];
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
}
