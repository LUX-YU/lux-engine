#pragma once
namespace lux::editor::views
{
    class ViewFactoryEntry;
    struct ViewFactoryDescriptor;
} // namespace lux::editor::views
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/assets/ModelImporter.hpp>
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
    extern const desktop::UiDescriptor kImportView;
    class ImportView final : public lux::ui::Pane
    {
    public:
        using Browse = cxx::move_only_function<void(lux::ui::PaneHandle)>;
        [[nodiscard]] static cxx::expected<std::unique_ptr<lux::ui::Pane>, desktop::UiFailure>
        createConfigured(services::ServiceResolver&, const desktop::UiCreateInfo&);
        object::TSignal<> browseRequested{*this};
        ImportView(object::ObjectDispatcherRef, lux::ui::PaneId, ProjectCatalogModel&, assets::ModelImporter&);
        ~ImportView() noexcept override;
        ImportView(const ImportView&) = delete;
        ImportView& operator=(const ImportView&) = delete;
        ImportView(ImportView&&) = delete;
        ImportView& operator=(ImportView&&) = delete;
        // UI intent delivery only; the connected owner performs native dialog admission.
        [[nodiscard]] EditorResult<void> requestBrowse();
        void setSource(std::filesystem::path);
        void showFailure(EditorFailure);
        [[nodiscard]] EditorResult<assets::ModelImportId> importModel(assets::ModelImportRequest);
        [[nodiscard]] EditorResult<assets::ModelImportId> reimportModel(AssetReference, std::filesystem::path = {});

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
        object::Connection request_connection_;
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
