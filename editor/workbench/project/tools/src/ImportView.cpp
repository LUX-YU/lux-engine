#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>
#include <lux/engine/editor/project/ImportView.hpp>
#include <lux/engine/ui/Element.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <random>

namespace lux::editor::project
{
    namespace
    {
        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.import"},
            "Import Assets",
            "File"
        };
        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{
            views::ViewTypeIdView{"lux.editor.import"},
            "Import Assets",
            cxx::typeToken<std::monostate>()
        };
    } // namespace
    struct ImportView::Impl final
    {
        enum class EAction
        {
            BROWSE,
            IMPORT,
            REIMPORT,
            REPLACE,
            RETRY,
            ABANDON,
            ACKNOWLEDGE
        };
        struct Content final : lux::ui::Element
        {
            Impl& data;
            Content(ImportView& pane, Impl& owner) : Element(pane, lux::ui::ElementId{"import"}), data(owner)
            {
                setStretch({1, 1});
            }
            void draw() noexcept override
            {
                ImGui::InputText("Source file", &data.source);
                if (ImGui::Button("Browse"))
                    data.intent = EAction::BROWSE;
                ImGui::InputText("Asset path in project", &data.destination);
                ImGui::InputFloat("Scale", &data.configuration.uniform_scale);
                ImGui::Checkbox("Left handed", &data.configuration.make_left_handed);
                ImGui::Checkbox("Import animations", &data.configuration.import_animations);
                if (ImGui::Button("Import model"))
                    data.intent = EAction::IMPORT;
                ImGui::SeparatorText("Reimport a model");
                for (const auto& row : data.catalog.assets())
                {
                    if (row.magic != asset::ModelAsset::primary_magic)
                        continue;
                    if (ImGui::Selectable(row.path.c_str(), data.selected && data.selected->asset == row.id))
                        data.selected = data.catalog.reference(row.id);
                }
                if (ImGui::Button("Reimport captured sources"))
                    data.intent = EAction::REIMPORT;
                if (ImGui::Button("Reimport using source file"))
                    data.intent = EAction::REPLACE;
                if (data.status)
                {
                    if (const auto* pending = std::get_if<assets::ModelImportPending>(&*data.status))
                        ImGui::Text(
                            "Stage %u: %zu files, %zu bytes",
                            unsigned(pending->stage),
                            pending->files,
                            pending->bytes
                        );
                    else if (const auto* error = std::get_if<EditorFailure>(&*data.status))
                    {
                        ImGui::TextWrapped("%s: %s", error->domain.c_str(), error->message.c_str());
                        if (ImGui::Button("Retry / reconcile"))
                            data.intent = EAction::RETRY;
                    }
                    else if (std::holds_alternative<assets::ModelImportSucceeded>(*data.status))
                        ImGui::TextUnformatted("Import published. The project catalog has been updated.");
                    else if (const auto* abandoned = std::get_if<assets::ModelImportAbandoned>(&*data.status))
                        ImGui::Text(
                            "Abandoned; %zu immutable files had already reached disk.",
                            abandoned->published_files
                        );
                    if (ImGui::Button("Abandon"))
                        data.intent = EAction::ABANDON;
                    ImGui::SameLine();
                    if (ImGui::Button("Acknowledge result"))
                        data.intent = EAction::ACKNOWLEDGE;
                }
                if (data.failure)
                    ImGui::TextWrapped("%s: %s", data.failure->domain.c_str(), data.failure->message.c_str());
            }
        } content;
        ProjectCatalogModel& model;
        assets::ModelImporter& importer;
        ProjectCatalogSnapshot catalog;
        std::optional<AssetReference> selected;
        std::optional<assets::VModelImportStatus> status;
        std::optional<EAction> intent;
        std::optional<EditorFailure> failure;
        std::string source, destination{"Content/Models/Model"};
        toolchain::ModelCookConfiguration configuration;
        Impl(ImportView& pane, ProjectCatalogModel& model, assets::ModelImporter& importer)
            : content(pane, *this), model(model), importer(importer)
        {
        }
    };
    ImportView::ImportView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        ProjectCatalogModel& catalog,
        assets::ModelImporter& importer
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{kFactoryDescriptor.type.name()}, "Import Assets"),
          impl_(std::make_unique<Impl>(*this, catalog, importer))
    {
        setContent(impl_->content);
    }
    ImportView::~ImportView() noexcept = default;
    void ImportView::setSource(std::filesystem::path file)
    {
        impl_->source = file.string();
    }
    void ImportView::showFailure(EditorFailure error)
    {
        impl_->failure = std::move(error);
    }
    EditorResult<assets::ModelImportId> ImportView::importModel(assets::ModelImportRequest request)
    {
        return impl_->importer.requestModel(request);
    }
    EditorResult<assets::ModelImportId> ImportView::reimportModel(
        AssetReference reference,
        std::filesystem::path replacement
    )
    {
        auto resolved = impl_->model.resolve(reference, asset::ModelAsset::primary_magic);
        if (!resolved)
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "import.target", 0, {}, resolved.error()}
            );
        return impl_->importer.reimportModel(reference.asset, replacement);
    }
    void ImportView::update() noexcept
    {
        auto version = impl_->model.version();
        if (!version)
            impl_->failure = EditorFailure{EEditorError::SOURCE_FAILURE, "import.catalog", 0, {}, version.error()};
        else if (*version != impl_->catalog.version())
        {
            auto catalog = impl_->model.snapshot();
            if (catalog)
                impl_->catalog = std::move(*catalog);
            else
                impl_->failure = EditorFailure{EEditorError::SOURCE_FAILURE, "import.catalog", 0, {}, catalog.error()};
        }
        if (auto action = std::exchange(impl_->intent, {}))
        {
            EditorResult<void> result;
            if (*action == Impl::EAction::BROWSE)
            {
                if (!emit(browseRequested).complete())
                    result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "import.browse.delivery"});
            }
            else if (*action == Impl::EAction::IMPORT)
            {
                std::mt19937 engine{std::random_device{}()};
                auto imported = importModel(
                    {asset::AssetId{uuids::uuid_random_generator{engine}()},
                     impl_->source,
                     impl_->destination,
                     impl_->configuration}
                );
                if (!imported)
                    result = cxx::unexpected(imported.error());
            }
            else if (*action == Impl::EAction::REIMPORT || *action == Impl::EAction::REPLACE)
            {
                if (!impl_->selected)
                    result = cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "import.selection"});
                else
                {
                    auto imported = reimportModel(
                        *impl_->selected,
                        *action == Impl::EAction::REPLACE ? std::filesystem::path{impl_->source}
                                                          : std::filesystem::path{}
                    );
                    if (!imported)
                        result = cxx::unexpected(imported.error());
                }
            }
            else if (auto id = impl_->importer.currentRequest())
            {
                if (*action == Impl::EAction::RETRY)
                    result = impl_->importer.retry(*id);
                else if (*action == Impl::EAction::ABANDON)
                    result = impl_->importer.abandon(*id);
                else
                    result = impl_->importer.acknowledge(*id);
            }
            if (!result)
                impl_->failure = std::move(result.error());
            else
                impl_->failure.reset();
        }
        if (auto id = impl_->importer.currentRequest())
        {
            auto status = impl_->importer.status(*id);
            if (status)
                impl_->status = std::move(*status);
            else
                impl_->failure = std::move(status.error());
        }
        else
            impl_->status.reset();
    }
} // namespace lux::editor::project

namespace lux::editor::project
{
    std::shared_ptr<views::ViewFactoryEntry> makeImportViewFactory(
        ProjectCatalogModel& catalog,
        assets::ModelImporter& importer,
        cxx::move_only_function<void(lux::ui::PaneId)> browse
    )
    {
        auto receiver = std::make_shared<cxx::move_only_function<void(lux::ui::PaneId)>>(std::move(browse));
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(
            lux::object::CodeLease::builtin(),
            [&catalog, &importer, receiver](const views::ViewFactoryInput& input
            ) -> views::ViewFactoryResult<views::DetachedView>
            {
                auto pane = std::make_unique<ImportView>(input.dispatcher(), input.paneId(), catalog, importer);
                auto connection = object::LuxObject::connect(
                    pane.get(),
                    &ImportView::browseRequested,
                    [id = input.paneId(), receiver]() noexcept { (*receiver)(id); }
                );
                if (!connection)
                    return cxx::unexpected(workbench::detail::viewFailure(connection.error()));
                views::DetachedView view{lux::object::CodeLease::builtin(), std::move(pane)};
                view.addConnection(std::move(*connection));
                return view;
            }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeImportCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kCommand, kFactoryDescriptor>(std::move(query), std::move(open));
    }

} // namespace lux::editor::project
