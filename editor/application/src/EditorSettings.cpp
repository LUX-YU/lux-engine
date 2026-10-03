#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/SettingsView.hpp>
namespace
{
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_settings{
        lux::editor::commands::CommandIdView{"lux.editor.settings"},
        "Project Settings",
        "Window"
    };
}
namespace lux::editor::application
{
    void EditorApplication::Impl::installSettingsView(extensions::ContributionDraft& draft)
    {
        draft.views.push_back(views::ViewFactoryEntry::create(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeIdView{"lux.editor.settings"},
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
        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_settings>(
            contracts::CodeLease::builtin(),
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
                if (phase_ != EApplicationPhase::RUNNING)
                    result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.publication"});
                else if (input)
                    result = plugin_saving_->request(input->based_on, std::move(input->desired));
                else
                    result = cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "settings.input"});
            }
            else
            {
                if (*action == EPluginAction::RETRY)
                    result = plugin_saving_->retry();
                else if (*action == EPluginAction::ABANDON)
                    result = plugin_saving_->abandon();
                else
                    result = plugin_saving_->acknowledge();
            }
            if (!result)
                plugin_failure_ = result.error();
            else
                plugin_failure_.reset();
        }
        if (auto updated = plugin_saving_->update(); !updated)
            return updated;
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
                else if (const auto* publication = plugin_saving_->status())
                    status = *publication;
                static_cast<project::SettingsView&>(pane).setPublicationStatus(std::move(status));
            };
            auto received = desktop_->views().withView(info.id, receive);
            if (!received)
                return applicationFailure("settings.status", received.error());
        }
        return {};
    }
}
