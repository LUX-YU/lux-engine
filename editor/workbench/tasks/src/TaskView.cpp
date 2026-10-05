#include <algorithm>
#include <exception>
#include <imgui.h>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>
namespace lux::editor::tasks
{
    namespace
    {
        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.tasks"},
            "Background Tasks",
            "Window"
        };
        constexpr services::ServiceDependency kDependencies[]{
            {services::ServiceNameView{"lux.editor.tasks.monitor"},
             1,
             cxx::typeToken<TaskMonitor>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        desktop::UiResult<std::unique_ptr<lux::ui::Pane>> createView(
            services::ServiceResolver& resolver,
            const desktop::UiCreateInfo& input
        )
        {
            const bool has_content = !input.content.sessions.empty();
            const bool has_configuration = !input.configuration.bytes.empty();
            const bool is_invalid_input = has_content || has_configuration;
            if (is_invalid_input)
            {
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::INVALID_CONFIGURATION,
                    "tasks.window",
                    0,
                    "The task window accepts no author binding or configuration payload"
                });
            }
            auto monitor = resolver.require<TaskMonitor>(0);
            if (!monitor)
            {
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::DEPENDENCY,
                    "tasks.monitor",
                    static_cast<std::uint64_t>(monitor.error().code),
                    monitor.error().detail
                });
            }
            return std::make_unique<TaskView>(input.dispatcher, input.instance, monitor->get());
        }
        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{
            views::ViewTypeIdView{"lux.editor.tasks"},
            "Tasks",
            cxx::typeToken<std::monostate>()
        };
    } // namespace
    constinit const desktop::UiDescriptor kTaskView{
        .type = kFactoryDescriptor.type,
        .label = kFactoryDescriptor.label,
        .dependencies = kDependencies,
        .create = createView
    };
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
        {
            std::terminate();
        }
        changes_ = std::move(*connected);
    }
    void TaskListElement::requestCancel(process::TaskId id)
    {
        if (std::ranges::find(cancel_, id) == cancel_.end())
        {
            cancel_.push_back(id);
        }
    }
    void TaskListElement::update() noexcept
    {
        rejected_.clear();
        for (auto id : cancel_)
        {
            if (!query_.requestCancel(id))
            {
                rejected_.push_back(id);
            }
        }
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
        {
            std::terminate(); // Fixed content in a detached Pane.
        }
    }
    void TaskListElement::draw() noexcept
    {
        constexpr const char* states[]{"Queued", "Running", "Succeeded", "Failed", "Cancelled"};
        if (!ImGui::BeginTable("tasks", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
        {
            return;
        }
        ImGui::TableSetupColumn("Task");
        ImGui::TableSetupColumn("Stage");
        ImGui::TableSetupColumn("Progress");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_->size()));
        while (clipper.Step())
        {
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
                {
                    ImGui::Text(
                        "%.0f%%",
                        100.0 * static_cast<double>(task.progress->completed) /
                            static_cast<double>(task.progress->total)
                    );
                }
                else
                {
                    ImGui::TextUnformatted("--");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(states[static_cast<unsigned>(task.state)]);
                ImGui::TableNextColumn();
                if (!task.finished)
                {
                    ImGui::BeginDisabled(task.cancel_requested);
                    if (ImGui::SmallButton("Cancel"))
                    {
                        cancel_.push_back(task.id);
                    }
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
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
            {
                return views::DetachedView{
                    object::CodeLease::builtin(),
                    std::make_unique<TaskView>(input.dispatcher(), input.paneId(), monitor)
                };
            }
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
