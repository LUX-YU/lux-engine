#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/SettingsView.hpp>
namespace lux::editor::application
{
    void EditorApplication::Impl::installSettingsView(extensions::ContributionDraft& draft)
    {
        draft.views.push_back(project::makeSettingsViewFactory(
            *project_,
            plugins_,
            [this](const project::PluginSelectionDraft& draft) noexcept
            {
                if (plugin_action_ || phase_ != EApplicationPhase::RUNNING)
                    plugin_failure_ = EditorFailure{EEditorError::BUSY, "settings.admission"};
                else
                {
                    plugin_selection_ = PluginSelection{draft.based_on, draft.desired};
                    plugin_action_ = EPluginAction::SAVE;
                }
            },
            [this]() noexcept
            {
                if (!plugin_action_)
                    plugin_action_ = EPluginAction::RETRY;
            },
            [this]() noexcept
            {
                if (!plugin_action_)
                    plugin_action_ = EPluginAction::ABANDON;
            },
            [this]() noexcept
            {
                if (!plugin_action_)
                    plugin_action_ = EPluginAction::ACKNOWLEDGE;
            },
            settings_content_
        ));
        draft.commands.push_back(project::makeSettingsCommand(
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; },
            toolOpening()
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
            auto receive = [&](lux::ui::Pane& pane)
            {
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
} // namespace lux::editor::application
