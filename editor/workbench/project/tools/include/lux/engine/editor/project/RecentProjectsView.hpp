#pragma once
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <filesystem>

namespace lux::editor
{
    class RecentProjects;
    class ProjectStorage;
    struct AssetReference;
}
namespace lux::editor::views { class ViewFactoryEntry; }

namespace lux::editor::project
{
    class RecentProjectsView final : public lux::ui::Pane
    {
    public:
        object::TSignal<std::filesystem::path> openRequested{*this};
        RecentProjectsView(object::ObjectDispatcherRef, lux::ui::PaneId, RecentProjects&);
        ~RecentProjectsView() noexcept override;
        RecentProjectsView(const RecentProjectsView&) = delete;
        RecentProjectsView& operator=(const RecentProjectsView&) = delete;
        RecentProjectsView(RecentProjectsView&&) = delete;
        RecentProjectsView& operator=(RecentProjectsView&&) = delete;
        void showFailure(EditorFailure);

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> makeRecentProjectsViewFactory(
        RecentProjects& recent, cxx::move_only_function<EditorResult<void>(const std::filesystem::path&)> open
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeRecentProjectsCommand(
        commands::CommandEntry::Query, desktop::ToolOpening
    );

    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeOpenProjectCommand(
        commands::CommandEntry::Query, cxx::move_only_function<commands::CommandResult<void>()>
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeInitialSceneCommand(
        commands::CommandEntry::Query, ProjectStorage&,
        cxx::move_only_function<commands::CommandResult<void>(AssetReference)>
    );

}
