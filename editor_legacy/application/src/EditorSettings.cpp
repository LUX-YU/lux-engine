#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/SettingsView.hpp>
namespace lux::editor::application
{
    void EditorApplication::Impl::installSettingsView(extensions::ContributionDraft& draft)
    {
        plugin_requests_.save = [this](const project::PluginSelectionDraft& draft) noexcept
        {
            if (plugin_action_ || phase_ != EApplicationPhase::RUNNING)
            {
                plugin_failure_ = EditorFailure{EEditorError::BUSY, "settings.admission"};
            }
            else
            {
                plugin_selection_ = PluginSelection{draft.based_on, draft.desired};
                plugin_action_ = EPluginAction::SAVE;
            }
        };
        plugin_requests_.retry = [this]() noexcept
        {
            if (!plugin_action_)
            {
                plugin_action_ = EPluginAction::RETRY;
            }
        };
        plugin_requests_.abandon = [this]() noexcept
        {
            if (!plugin_action_)
            {
                plugin_action_ = EPluginAction::ABANDON;
            }
        };
        plugin_requests_.acknowledge = [this]() noexcept
        {
            if (!plugin_action_)
            {
                plugin_action_ = EPluginAction::ACKNOWLEDGE;
            }
        };
        draft.ui.push_back(desktop::UiEntry::bind<project::kSettingsView>(object::CodeLease::builtin()));
        draft.commands.push_back(project::makeSettingsCommand(
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; },
            toolOpening()
        ));
    }
    EditorResult<void> EditorApplication::Impl::maintainProjectSettings()
    {
        // First use is a creation boundary. A temporarily unavailable factory must not consume
        // the pending action or its original source selection.
        const bool needs_selection = plugin_action_.has_value() && !plugin_saving_;
        if (needs_selection && phase_ == EApplicationPhase::RUNNING)
        {
            auto selection = editor_context_.services().get<ProjectPluginSelection>(editor_context_.scope());
            if (!selection)
            {
                return applicationFailure("settings.plugin-service", selection.error());
            }
            plugin_saving_ = std::move(*selection);
        }
        if (auto action = std::exchange(plugin_action_, {}))
        {
            EditorResult<void> result;
            if (!plugin_saving_)
            {
                plugin_selection_.reset();
                result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.publication"});
            }
            else if (*action == EPluginAction::SAVE)
            {
                auto input = std::exchange(plugin_selection_, {});
                if (phase_ != EApplicationPhase::RUNNING)
                {
                    result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.publication"});
                }
                else if (input)
                {
                    result = plugin_saving_->request(input->based_on, std::move(input->desired));
                }
                else
                {
                    result = cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "settings.input"});
                }
            }
            else
            {
                if (*action == EPluginAction::RETRY)
                {
                    result = plugin_saving_->retry();
                }
                else if (*action == EPluginAction::ABANDON)
                {
                    result = plugin_saving_->abandon();
                }
                else
                {
                    result = plugin_saving_->acknowledge();
                }
            }
            if (!result)
            {
                plugin_failure_ = result.error();
            }
            else
            {
                plugin_failure_.reset();
            }
        }
        if (!desktop_)
        {
            return {};
        }
        auto all = editor_context_.ui().describe(desktop_->root());
        if (!all)
        {
            return applicationFailure("settings.views", all.error());
        }
        for (const auto& info : *all)
        {
            if (info.type.view() != project::kSettingsView.type)
            {
                continue;
            }
            auto receive = [&](lux::ui::Pane& pane)
            {
                std::optional<VPublicationStatus> status;
                if (plugin_failure_)
                {
                    status = *plugin_failure_;
                }
                else if (const auto* publication = plugin_saving_ ? plugin_saving_->status() : nullptr)
                {
                    status = *publication;
                }
                static_cast<project::SettingsView&>(pane).setPublicationStatus(std::move(status));
            };
            auto received = desktop_->root().withPane(info.handle, receive);
            if (!received)
            {
                return applicationFailure("settings.status", received.error());
            }
        }
        return {};
    }
} // namespace lux::editor::application
