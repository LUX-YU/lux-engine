#pragma once
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/project/ProjectCatalogModel.hpp>
#include <lux/engine/editor/assets/ModelImporter.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::views
{
    class ViewFactoryEntry;
}

namespace lux::editor::project
{
    class ImportView final : public lux::ui::Pane
    {
    public:
        object::TSignal<> browseRequested{*this};
        ImportView(object::ObjectDispatcherRef, lux::ui::PaneId, ProjectCatalogModel&, assets::ModelImporter&);
        ~ImportView() noexcept override;
        ImportView(const ImportView&) = delete;
        ImportView& operator=(const ImportView&) = delete;
        ImportView(ImportView&&) = delete;
        ImportView& operator=(ImportView&&) = delete;
        void setSource(std::filesystem::path);
        void showFailure(EditorFailure);
        [[nodiscard]] EditorResult<assets::ModelImportId> importModel(assets::ModelImportRequest);
        [[nodiscard]] EditorResult<assets::ModelImportId> reimportModel(AssetReference, std::filesystem::path = {});

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> makeImportViewFactory(
        ProjectCatalogModel& catalog,
        assets::ModelImporter& importer,
        cxx::move_only_function<void(lux::ui::PaneId)> browse
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeImportCommand(
        commands::CommandEntry::Query,
        desktop::ToolOpening
    );

} // namespace lux::editor::project
