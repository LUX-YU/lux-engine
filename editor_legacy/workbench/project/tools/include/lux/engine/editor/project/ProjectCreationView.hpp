#pragma once
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/project/ProjectCreation.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::desktop
{
    struct UiDescriptor;
}

namespace lux::editor::project
{
    extern const desktop::UiDescriptor kProjectCreationView;
    class ProjectCreationView final : public lux::ui::Pane
    {
    public:
        ProjectCreationView(object::ObjectDispatcherRef, lux::ui::PaneId, std::shared_ptr<ProjectCreation>, EditorResult<void>&);
        ~ProjectCreationView() noexcept override;
        ProjectCreationView(const ProjectCreationView&) = delete;
        ProjectCreationView& operator=(const ProjectCreationView&) = delete;
        ProjectCreationView(ProjectCreationView&&) = delete;
        ProjectCreationView& operator=(ProjectCreationView&&) = delete;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeProjectCreationCommand(
        commands::CommandEntry::Query,
        desktop::ToolOpening
    );

} // namespace lux::editor::project
