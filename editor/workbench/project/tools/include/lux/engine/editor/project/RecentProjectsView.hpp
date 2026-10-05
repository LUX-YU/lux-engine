#pragma once
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/desktop/ViewCommands.hpp>

#include <filesystem>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor
{
    class RecentProjects;
    class ProjectStorage;
    struct AssetReference;
} // namespace lux::editor

namespace lux::editor::desktop
{
    struct UiDescriptor;
    struct UiFailure;
    struct UiCreateInfo;
} // namespace lux::editor::desktop

namespace lux::services
{
    class ServiceResolver;
}

namespace lux::editor::project
{
    extern const desktop::UiDescriptor kRecentProjectsView;
    class RecentProjectsView final : public lux::ui::Pane
    {
    public:
        using Open = cxx::move_only_function<EditorResult<void>(const std::filesystem::path&)>;
        [[nodiscard]] static cxx::expected<std::unique_ptr<lux::ui::Pane>, desktop::UiFailure>
        createConfigured(services::ServiceResolver&, const desktop::UiCreateInfo&);
        object::TSignal<std::filesystem::path> openRequested{*this};
        RecentProjectsView(object::ObjectDispatcherRef, lux::ui::PaneId, RecentProjects&);
        ~RecentProjectsView() noexcept override;
        RecentProjectsView(const RecentProjectsView&) = delete;
        RecentProjectsView& operator=(const RecentProjectsView&) = delete;
        RecentProjectsView(RecentProjectsView&&) = delete;
        RecentProjectsView& operator=(RecentProjectsView&&) = delete;
        // Delivers the intent, not a claim that the asynchronous project launch was admitted.
        [[nodiscard]] EditorResult<void> requestOpen(std::filesystem::path);
        void showFailure(EditorFailure);

    private:
        void update() noexcept override;
        struct Impl;
        std::shared_ptr<RecentProjects> projects_owner_;
        std::unique_ptr<Impl> impl_;
        object::Connection request_connection_;
    };
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeRecentProjectsCommand(
        commands::CommandEntry::Query,
        desktop::ToolOpening
    );

    [[nodiscard]] std::shared_ptr<commands::CommandEntry>
        makeOpenProjectCommand(commands::CommandEntry::Query, cxx::move_only_function<commands::CommandResult<void>()>);
    [[nodiscard]] std::shared_ptr<commands::CommandEntry>
    makeInitialSceneCommand(commands::CommandEntry::Query, ProjectStorage&, cxx::move_only_function<commands::CommandResult<void>(AssetReference)>);

} // namespace lux::editor::project
