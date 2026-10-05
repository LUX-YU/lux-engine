#pragma once
namespace lux::editor::views
{
    class ViewFactoryEntry;
    struct ViewFactoryDescriptor;
} // namespace lux::editor::views
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/project/ProjectCatalogModel.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::views
{
    class ViewFactoryEntry;
}

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
    extern const desktop::UiDescriptor kProjectView;
    class ProjectView final : public lux::ui::Pane
    {
    public:
        using Open = cxx::move_only_function<void(const AssetReference&)>;
        [[nodiscard]] static cxx::expected<std::unique_ptr<lux::ui::Pane>, desktop::UiFailure>
        createConfigured(services::ServiceResolver&, const desktop::UiCreateInfo&);
        object::TSignal<AssetReference> openRequested{*this};
        ProjectView(object::ObjectDispatcherRef, lux::ui::PaneId, ProjectCatalogModel&);
        ~ProjectView() noexcept override;
        ProjectView(const ProjectView&) = delete;
        ProjectView& operator=(const ProjectView&) = delete;
        ProjectView(ProjectView&&) = delete;
        ProjectView& operator=(ProjectView&&) = delete;
        [[nodiscard]] ProjectQueryResult<void> refresh();
        [[nodiscard]] ProjectQueryResult<void> requestOpen(AssetReference);
        [[nodiscard]] const ProjectCatalogSnapshot& catalog() const noexcept;
        [[nodiscard]] const std::optional<VProjectQueryFailure>& status() const noexcept;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
        object::Connection request_connection_;
    };
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> makeProjectViewFactory(
        ProjectCatalogModel& catalog,
        cxx::move_only_function<void(const AssetReference&)> open
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeAssetsCommand(
        commands::CommandEntry::Query,
        desktop::ToolOpening
    );

} // namespace lux::editor::project
