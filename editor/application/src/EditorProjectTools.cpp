#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/ImportView.hpp>
#include <lux/engine/window/FileDialog.hpp>

namespace lux::editor::application
{
    void EditorApplication::Impl::installProjectTools(extensions::ContributionDraft& draft)
    {
        installSettingsView(draft);
        installProjectCreation(draft);
        draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.import"},
                "Import Assets",
                cxx::typeToken<EmptyViewInput>()
            },
            [this](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                std::erase_if(connections_, [](const auto& value) { return !value.connected(); });
                if (connections_.size() >= 64)
                    return cxx::unexpected(
                        views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "import.connections"}
                    );
                auto view = std::make_unique<project::ImportView>(
                    input.dispatcher(),
                    input.paneId(),
                    project_->catalogModel(),
                    *importer_
                );
                auto connected = object::LuxObject::connect(
                    view.get(),
                    &project::ImportView::browseRequested,
                    [this, pane = input.paneId()]() noexcept { import_browse_ = pane; }
                );
                if (!connected)
                    return cxx::unexpected(
                        views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "import.browse"}
                    );
                connections_.push_back(std::move(*connected));
                return views::DetachedView{contracts::CodeLease::builtin(), std::move(view)};
            }
        ));
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{commands::CommandId{"lux.editor.import"}, "Import Assets", "File"},
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                auto shown = showTool(views::ViewTypeId{"lux.editor.import"});
                if (!shown)
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE,
                        shown.error().domain,
                        shown.error().reason,
                        shown.error().message
                    });
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
    }
    EditorResult<void> EditorApplication::Impl::receiveProjectIntents()
    {
        if (!import_browse_)
            return {};
        const auto target = std::exchange(import_browse_, {});
        auto all = desktop_->views().describeAll();
        if (!all)
            return applicationFailure("import.browse.views", all.error());
        std::optional<views::ViewId> found;
        for (const auto& view : *all)
        {
            auto compare = [&](lux::ui::Pane& pane) {
                if (pane.id() == *target && pane.type() == lux::ui::PaneTypeId{"lux.editor.import"})
                    found = view.id;
            };
            auto visited = desktop_->views().withView(view.id, compare);
            if (!visited)
                return applicationFailure("import.browse.target", visited.error());
        }
        if (!found)
            return {}; // A closed UI cannot redirect its native result to a later window.
        auto chosen = window::openFileDialog(window_.get());
        auto deliver = [&](lux::ui::Pane& pane) {
            auto& view = static_cast<project::ImportView&>(pane);
            if (!chosen)
                view.showFailure(EditorFailure{EEditorError::SOURCE_FAILURE, "import.browse", 0, chosen.error().detail}
                );
            else if (*chosen)
                view.setSource(std::move(**chosen));
        };
        auto delivered = desktop_->views().withView(*found, deliver);
        if (!delivered)
            return applicationFailure("import.browse.deliver", delivered.error());
        return {};
    }
}
