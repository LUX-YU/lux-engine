#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/SettingsView.hpp>
namespace lux::editor::application
{
    void EditorApplication::Impl::installSettingsView(extensions::ContributionDraft& draft)
    {
        draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.settings"},
                "Settings",
                cxx::typeToken<std::monostate>()
            },
            [this](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                std::erase_if(connections_, [](const auto& value) { return !value.connected(); });
                if (connections_.size() > 60)
                    return cxx::unexpected(
                        views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "settings.connections"}
                    );
                auto view =
                    std::make_unique<project::SettingsView>(input.dispatcher(), input.paneId(), *project_, plugins_);
                std::array<object::LuxObject::ConnectResult, 4> bindings{
                    object::LuxObject::connect(
                        view.get(),
                        &project::SettingsView::selectionRequested,
                        [this](const project::PluginSelectionDraft& draft) noexcept {
                            if (plugin_action_ || phase_ != EApplicationPhase::RUNNING)
                                plugin_failure_ = EditorFailure{EEditorError::BUSY, "settings.admission"};
                            else
                            {
                                plugin_selection_ = PluginSelection{draft.based_on, draft.desired};
                                plugin_action_ = EPluginAction::SAVE;
                            }
                        }
                    ),
                    object::LuxObject::connect(
                        view.get(),
                        &project::SettingsView::retryRequested,
                        [this]() noexcept {
                            if (!plugin_action_)
                                plugin_action_ = EPluginAction::RETRY;
                        }
                    ),
                    object::LuxObject::connect(
                        view.get(),
                        &project::SettingsView::abandonRequested,
                        [this]() noexcept {
                            if (!plugin_action_)
                                plugin_action_ = EPluginAction::ABANDON;
                        }
                    ),
                    object::LuxObject::connect(
                        view.get(),
                        &project::SettingsView::acknowledgeRequested,
                        [this]() noexcept {
                            if (!plugin_action_)
                                plugin_action_ = EPluginAction::ACKNOWLEDGE;
                        }
                    )
                };
                for (const auto& binding : bindings)
                    if (!binding)
                        return cxx::unexpected(
                            views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "settings.connect"}
                        );
                for (auto& binding : bindings)
                    connections_.push_back(std::move(*binding));
                return views::DetachedView{contracts::CodeLease::builtin(), std::move(view)};
            }
        ));
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{commands::CommandId{"lux.editor.settings"}, "Project Settings", "Window"},
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                auto shown = showTool(views::ViewTypeId{"lux.editor.settings"});
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
    EditorResult<void> EditorApplication::Impl::maintainProjectSettings()
    {
        if (auto action = std::exchange(plugin_action_, {}))
        {
            EditorResult<void> result;
            if (*action == EPluginAction::SAVE)
            {
                auto input = std::exchange(plugin_selection_, {});
                if (phase_ != EApplicationPhase::RUNNING || plugin_publication_)
                    result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.publication"});
                else if (!input || input->based_on != project_->manifest().plugins)
                    result = cxx::unexpected(EditorFailure{
                        EEditorError::STALE_REQUEST,
                        "settings.source",
                        0,
                        "Project selection changed. Revert the draft before trying again."
                    });
                else
                {
                    ProjectUpdate update;
                    update.plugins = std::move(input->desired);
                    auto publication = project_->preparePublication(update);
                    if (!publication)
                        result = cxx::unexpected(publication.error());
                    else
                        plugin_publication_ = std::make_unique<ProjectPublicationOperation>(
                            *project_,
                            engine_->execution(),
                            writes_,
                            files_,
                            save_execution_,
                            std::move(*publication)
                        );
                }
            }
            else if (plugin_publication_)
            {
                if (*action == EPluginAction::RETRY)
                    result = plugin_publication_->retry();
                else if (*action == EPluginAction::ABANDON)
                    plugin_publication_->abandon();
                else if (!plugin_publication_->terminal())
                    result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.acknowledge"});
                else
                    plugin_publication_.reset();
            }
            if (!result)
                plugin_failure_ = result.error();
            else
                plugin_failure_.reset();
        }
        if (plugin_publication_)
            plugin_publication_->update();
        if (!desktop_)
            return {};
        auto all = desktop_->views().describeAll();
        if (!all)
            return applicationFailure("settings.views", all.error());
        for (const auto& info : *all)
        {
            if (info.type != views::ViewTypeId{"lux.editor.settings"})
                continue;
            auto receive = [&](lux::ui::Pane& pane) {
                std::optional<VPublicationStatus> status;
                if (plugin_failure_)
                    status = *plugin_failure_;
                else if (plugin_publication_)
                    status = plugin_publication_->status();
                static_cast<project::SettingsView&>(pane).setPublicationStatus(std::move(status));
            };
            auto received = desktop_->views().withView(info.id, receive);
            if (!received)
                return applicationFailure("settings.status", received.error());
        }
        return {};
    }
}
