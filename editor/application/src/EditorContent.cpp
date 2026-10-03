#include <lux/engine/editor/application/EditorApplicationImpl.hpp>

namespace
{
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_close_view{
        lux::editor::commands::CommandIdView{"lux.editor.close-view"},
        "Close View",
        "Window",
        "Ctrl+W",
        lux::editor::commands::ECommandScope::VIEW
    };
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_another_view{
        lux::editor::commands::CommandIdView{"lux.editor.another-view"},
        "Another View",
        "Window",
        "",
        lux::editor::commands::ECommandScope::SESSION
    };
}
namespace lux::editor::application
{
    EditorResult<sessions::OpenAssetId> EditorApplication::Impl::createContent(sessions::SessionPreparation data)
    {
        if (phase_ != EApplicationPhase::RUNNING)
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "content.create"});
        if (opens_.size() >= 64)
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "content.create"});
        auto installed = opening_.create(
            project_->catalogModel().reference({}).project_instance, std::move(data), contributions_.snapshot().sessions()
        );
        if (!installed)
            return applicationFailure("content.create", installed.error());
        opens_.push_back({*installed});
        return *installed;
    }
    void EditorApplication::Impl::installContentCommands(extensions::ContributionDraft& draft)
    {
        const auto running = [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
            return commands::CommandState{phase_ == EApplicationPhase::RUNNING && !last_view_};
        };
        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_close_view>(
            contracts::CodeLease::builtin(),
            running,
            [this](const commands::CommandInvocation& invocation
            ) -> commands::CommandResult<commands::DispatchReceipt> {
                auto result = closeView(std::get<views::ViewId>(invocation.target()));
                if (!result)
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE,
                        result.error().domain,
                        result.error().reason,
                        result.error().message
                    });
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_another_view>(
            contracts::CodeLease::builtin(),
            running,
            [this](const commands::CommandInvocation& invocation
            ) -> commands::CommandResult<commands::DispatchReceipt> {
                auto result = makeContentView(
                    {{std::get<commands::SessionTarget>(invocation.target()).id},
                     std::get<commands::SessionTarget>(invocation.target()).id},
                    true,
                    contributions_.snapshot()
                );
                if (!result)
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE,
                        result.error().domain,
                        result.error().reason,
                        result.error().message
                    });
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        for (bool project : {true, false})
            draft.commands.push_back(commands::CommandEntry::create(
                contracts::CodeLease::builtin(),
                commands::CommandDescriptor{
                    commands::CommandIdView{project ? "lux.editor.assets" : "lux.editor.tasks"},
                    project ? "Assets" : "Background Tasks",
                    "Window"
                },
                running,
                [this,
                 project](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                    auto result = showTool(views::ViewTypeId{project ? "lux.editor.project" : "lux.editor.tasks"});
                    if (!result)
                        return cxx::unexpected(commands::CommandFailure{
                            commands::ECommandError::DOMAIN_FAILURE,
                            result.error().domain,
                            result.error().reason,
                            result.error().message
                        });
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }
            ));
        auto creation_available =
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING && opens_.size() < 64};
            };
        draft.commands.push_back(material::makeNewMaterialCommand(creation_available, contentCreation()));
        draft.commands.push_back(flowforge::makeNewFlowCommand(creation_available, contentCreation(), flow_environment_));
    }
    sessions::SessionCreation EditorApplication::Impl::contentCreation()
    {
        return [this](sessions::SessionPreparation prepared) -> commands::CommandResult<commands::DispatchReceipt> {
            auto opened = createContent(std::move(prepared));
            if (!opened)
            {
                const auto& error = opened.error();
                const auto code = error.code == EEditorError::BUSY
                    ? commands::ECommandError::BUSY : commands::ECommandError::DOMAIN_FAILURE;
                return cxx::unexpected(commands::CommandFailure{code, error.domain, error.reason, error.message});
            }
            return commands::DispatchReceipt{commands::AcceptedOperation{"open", opened->value}};
        };
    }
}
