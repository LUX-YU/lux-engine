#include <exception>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/project/ImportView.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Root.hpp>
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
        constexpr services::ServiceDependency kDependencies[]{
            {services::ServiceNameView{"lux.editor.project.catalog"},
             1,
             cxx::typeToken<ProjectCatalogModel>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.assets.importer"},
             1,
             cxx::typeToken<assets::ModelImporter>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.project.import.browse"},
             1,
             cxx::typeToken<ImportView::Browse>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT,
             {},
             {},
             true}
        };
    } // namespace
    desktop::UiResult<std::unique_ptr<lux::ui::Pane>> ImportView::createConfigured(
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
                "project.tool",
                0,
                "This window accepts no author binding or configuration payload"
            });
        }
        auto receiver = resolver.require<Browse>(2);
        const bool has_receiver_failure = !receiver && receiver.error().code != services::EServiceError::NOT_FOUND;
        if (has_receiver_failure)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::DEPENDENCY,
                "lux.editor.project.import.browse",
                static_cast<std::uint64_t>(receiver.error().code),
                receiver.error().detail
            });
        }
        const bool is_empty_receiver = receiver && !receiver->get();
        if (is_empty_receiver)
        {
            return cxx::unexpected(
                desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "lux.editor.project.import.browse"}
            );
        }
        auto catalog = resolver.require<ProjectCatalogModel>(0);
        if (!catalog)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::DEPENDENCY,
                "lux.editor.project.catalog",
                static_cast<std::uint64_t>(catalog.error().code),
                catalog.error().detail
            });
        }
        auto importer = resolver.get<assets::ModelImporter>(1);
        if (!importer)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::DEPENDENCY,
                "lux.editor.assets.importer",
                static_cast<std::uint64_t>(importer.error().code),
                importer.error().detail
            });
        }
        auto pane = std::make_unique<ImportView>(input.dispatcher, input.instance, catalog->get(), **importer);
        pane->importer_owner_ = std::move(*importer);
        if (receiver)
        {
            auto connection = object::LuxObject::connect(
                pane.get(),
                &ImportView::browseRequested,
                [receiver = *receiver, target = pane.get()]() noexcept
                {
                    auto* root = target->attachedRoot();
                    if (!root)
                    {
                        target->showFailure(EditorFailure{EEditorError::INVALID_STATE, "import.browse.unmounted"});
                        return;
                    }
                    auto id = root->identify(*target);
                    if (!id)
                    {
                        target->showFailure(EditorFailure{EEditorError::STALE_REQUEST, "import.browse.identity"});
                        return;
                    }
                    receiver.get()(*id);
                }
            );
            if (!connection)
            {
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::FACTORY_FAILURE,
                    "lux.editor.project.import.browse",
                    static_cast<std::uint64_t>(connection.error())
                });
            }
            pane->request_connection_ = std::move(*connection);
        }
        return pane;
    }
    constinit const desktop::UiDescriptor kImportView{
        .type = views::ViewTypeIdView{"lux.editor.import"},
        .label = "Import Assets",
        .dependencies = kDependencies,
        .create = ImportView::createConfigured
    };
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
                {
                    data.intent = EAction::BROWSE;
                }
                ImGui::InputText("Asset path in project", &data.destination);
                ImGui::InputFloat("Scale", &data.configuration.uniform_scale);
                ImGui::Checkbox("Left handed", &data.configuration.make_left_handed);
                ImGui::Checkbox("Import animations", &data.configuration.import_animations);
                if (ImGui::Button("Import model"))
                {
                    data.intent = EAction::IMPORT;
                }
                ImGui::SeparatorText("Reimport a model");
                for (const auto& row : data.catalog.assets())
                {
                    if (row.magic != asset::ModelAsset::primary_magic)
                    {
                        continue;
                    }
                    if (ImGui::Selectable(row.path.c_str(), data.selected && data.selected->asset == row.id))
                    {
                        data.selected = data.catalog.reference(row.id);
                    }
                }
                if (ImGui::Button("Reimport captured sources"))
                {
                    data.intent = EAction::REIMPORT;
                }
                if (ImGui::Button("Reimport using source file"))
                {
                    data.intent = EAction::REPLACE;
                }
                if (data.status)
                {
                    if (const auto* pending = std::get_if<assets::ModelImportPending>(&*data.status))
                    {
                        ImGui::Text(
                            "Stage %u: %zu files, %zu bytes",
                            unsigned(pending->stage),
                            pending->files,
                            pending->bytes
                        );
                    }
                    else if (const auto* error = std::get_if<EditorFailure>(&*data.status))
                    {
                        ImGui::TextWrapped("%s: %s", error->domain.c_str(), error->message.c_str());
                        if (ImGui::Button("Retry / reconcile"))
                        {
                            data.intent = EAction::RETRY;
                        }
                    }
                    else if (std::holds_alternative<assets::ModelImportSucceeded>(*data.status))
                    {
                        ImGui::TextUnformatted("Import published. The project catalog has been updated.");
                    }
                    else if (const auto* abandoned = std::get_if<assets::ModelImportAbandoned>(&*data.status))
                    {
                        ImGui::Text(
                            "Abandoned; %zu immutable files had already reached disk.",
                            abandoned->published_files
                        );
                    }
                    if (ImGui::Button("Abandon"))
                    {
                        data.intent = EAction::ABANDON;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Acknowledge result"))
                    {
                        data.intent = EAction::ACKNOWLEDGE;
                    }
                }
                if (data.failure)
                {
                    ImGui::TextWrapped("%s: %s", data.failure->domain.c_str(), data.failure->message.c_str());
                }
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
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{kImportView.type.name()}, "Import Assets"),
          impl_(std::make_unique<Impl>(*this, catalog, importer))
    {
        if (!setContent(impl_->content))
        {
            std::terminate(); // Fixed content in a detached Pane.
        }
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
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "import.target", 0, {}, resolved.error()}
            );
        }
        return impl_->importer.reimportModel(reference.asset, replacement);
    }
    EditorResult<void> ImportView::requestBrowse()
    {
        if (!attachedRoot())
        {
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "import.browse.unmounted"});
        }
        if (!emit(browseRequested).complete())
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "import.browse.delivery"});
        }
        return {};
    }
    void ImportView::update() noexcept
    {
        auto version = impl_->model.version();
        if (!version)
        {
            impl_->failure = EditorFailure{EEditorError::SOURCE_FAILURE, "import.catalog", 0, {}, version.error()};
        }
        else if (*version != impl_->catalog.version())
        {
            auto catalog = impl_->model.snapshot();
            if (catalog)
            {
                impl_->catalog = std::move(*catalog);
            }
            else
            {
                impl_->failure = EditorFailure{EEditorError::SOURCE_FAILURE, "import.catalog", 0, {}, catalog.error()};
            }
        }
        if (auto action = std::exchange(impl_->intent, {}))
        {
            EditorResult<void> result;
            if (*action == Impl::EAction::BROWSE)
            {
                result = requestBrowse();
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
                {
                    result = cxx::unexpected(imported.error());
                }
            }
            else if (*action == Impl::EAction::REIMPORT || *action == Impl::EAction::REPLACE)
            {
                if (!impl_->selected)
                {
                    result = cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "import.selection"});
                }
                else
                {
                    auto imported = reimportModel(
                        *impl_->selected,
                        *action == Impl::EAction::REPLACE ? std::filesystem::path{impl_->source}
                                                          : std::filesystem::path{}
                    );
                    if (!imported)
                    {
                        result = cxx::unexpected(imported.error());
                    }
                }
            }
            else if (auto id = impl_->importer.currentRequest())
            {
                if (*action == Impl::EAction::RETRY)
                {
                    result = impl_->importer.retry(*id);
                }
                else if (*action == Impl::EAction::ABANDON)
                {
                    result = impl_->importer.abandon(*id);
                }
                else
                {
                    result = impl_->importer.acknowledge(*id);
                }
            }
            if (!result)
            {
                impl_->failure = std::move(result.error());
            }
            else
            {
                impl_->failure.reset();
            }
        }
        if (auto id = impl_->importer.currentRequest())
        {
            auto status = impl_->importer.status(*id);
            if (status)
            {
                impl_->status = std::move(*status);
            }
            else
            {
                impl_->failure = std::move(status.error());
            }
        }
        else
        {
            impl_->status.reset();
        }
    }
} // namespace lux::editor::project

namespace lux::editor::project
{
    std::shared_ptr<commands::CommandEntry> makeImportCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kCommand, kImportView>(std::move(query), std::move(open));
    }

} // namespace lux::editor::project
