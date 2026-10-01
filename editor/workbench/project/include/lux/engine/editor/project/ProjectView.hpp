#pragma once
#include <lux/engine/editor/project/ProjectCatalogModel.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::project
{
    class ProjectView final : public lux::ui::Pane
    {
    public:
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
    };
    [[nodiscard]] views::DetachedView
    makeProjectView(object::ObjectDispatcherRef, lux::ui::PaneId, ProjectCatalogModel&);
}
