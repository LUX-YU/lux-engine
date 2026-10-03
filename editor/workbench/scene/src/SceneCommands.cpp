#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/scene/RunStore.hpp>
#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>

namespace lux::editor::scene
{
    namespace
    {
        constexpr commands::CommandDescriptor kOutliner{
            commands::CommandIdView{"lux.editor.scene.outliner"}, "outliner", "Scene", "",
            commands::ECommandScope::VIEW
        };
        constexpr commands::CommandDescriptor kInspector{
            commands::CommandIdView{"lux.editor.scene.inspector"}, "inspector", "Scene", "",
            commands::ECommandScope::VIEW
        };
        constexpr commands::CommandDescriptor kResources{
            commands::CommandIdView{"lux.editor.scene.resources"}, "resources", "Scene", "",
            commands::ECommandScope::VIEW
        };
        constexpr commands::CommandDescriptor kConfiguration{
            commands::CommandIdView{"lux.editor.scene.configuration"}, "configuration", "Scene", "",
            commands::ECommandScope::VIEW
        };
        constexpr commands::CommandDescriptor kPause{
            commands::CommandIdView{"lux.editor.scene.pause"}, "pause", "Scene", "",
            commands::ECommandScope::VIEW
        };
        constexpr commands::CommandDescriptor kResume{
            commands::CommandIdView{"lux.editor.scene.resume"}, "resume", "Scene", "",
            commands::ECommandScope::VIEW
        };
        constexpr commands::CommandDescriptor kStep{
            commands::CommandIdView{"lux.editor.scene.step"}, "step", "Scene", "",
            commands::ECommandScope::VIEW
        };
        constexpr commands::CommandDescriptor kStop{
            commands::CommandIdView{"lux.editor.scene.stop"}, "stop", "Scene", "",
            commands::ECommandScope::VIEW
        };
        constexpr commands::CommandDescriptor kPlay{
            commands::CommandIdView{"lux.editor.play"}, "Play Frozen Scene", "Scene", "Ctrl+P",
            commands::ECommandScope::SESSION
        };

        template <class Error> auto commandFailure(const Error& error)
        {
            auto detail = workbench::detail::viewFailure(error);
            return cxx::unexpected(commands::CommandFailure{
                detail.code == views::EViewFactoryError::BUSY ? commands::ECommandError::BUSY
                                                            : commands::ECommandError::DOMAIN_FAILURE,
                std::move(detail.domain), detail.domain_code, std::move(detail.detail)
            });
        }
        template <const commands::CommandDescriptor& Descriptor, class Action>
        std::shared_ptr<commands::CommandEntry> bindViewCommand(
            std::shared_ptr<commands::CommandEntry::Query> query, Action action
        )
        {
            return commands::CommandEntry::bind<Descriptor>(
                contracts::CodeLease::builtin(),
                [query](const commands::CommandQuery& input) { return (*query)(input); },
                [action = std::move(action)](const commands::CommandInvocation& input) mutable
                    -> commands::CommandResult<commands::DispatchReceipt> {
                    auto result = action(std::get<views::ViewId>(input.target()));
                    if (!result)
                        return cxx::unexpected(result.error());
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }
            );
        }
        template <class Action> auto forRun(desktop::ViewHost& host, Action action)
        {
            return [&host, action = std::move(action)](views::ViewId view) mutable -> commands::CommandResult<void> {
                auto group = shareSceneInteraction(host, view);
                if (!group)
                    return commandFailure(group.error());
                const auto run = (*group)->run();
                if (!run)
                    return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET, "run.view"});
                return action(*run);
            };
        }
    }
    std::vector<std::shared_ptr<commands::CommandEntry>> makeSceneToolCommands(
        commands::CommandEntry::Query query,
        cxx::move_only_function<commands::CommandResult<void>(views::ViewId, ESceneTool)> receiver
    )
    {
        auto check = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        auto show = std::make_shared<decltype(receiver)>(std::move(receiver));
        return {
            bindViewCommand<kOutliner>(check, [show](views::ViewId id) { return (*show)(id, ESceneTool::OUTLINER); }),
            bindViewCommand<kInspector>(check, [show](views::ViewId id) { return (*show)(id, ESceneTool::INSPECTOR); }),
            bindViewCommand<kResources>(check, [show](views::ViewId id) { return (*show)(id, ESceneTool::RESOURCES); }),
            bindViewCommand<kConfiguration>(check, [show](views::ViewId id) {
                return (*show)(id, ESceneTool::CONFIGURATION);
            })
        };
    }
    std::vector<std::shared_ptr<commands::CommandEntry>> makeRunViewCommands(
        commands::CommandEntry::Query query,
        desktop::ViewHost& host,
        RunStore& runs,
        cxx::move_only_function<commands::CommandResult<void>(RunId)> step,
        cxx::move_only_function<commands::CommandResult<void>(RunId)> stop
    )
    {
        auto check = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        return {
            bindViewCommand<kPause>(check, forRun(host, [&runs](RunId id) -> commands::CommandResult<void> {
                auto result = runs.pause(id);
                if (!result)
                    return commandFailure(result.error());
                return {};
            })),
            bindViewCommand<kResume>(check, forRun(host, [&runs](RunId id) -> commands::CommandResult<void> {
                auto result = runs.resume(id);
                if (!result)
                    return commandFailure(result.error());
                return {};
            })),
            bindViewCommand<kStep>(check, forRun(host, std::move(step))),
            bindViewCommand<kStop>(check, forRun(host, std::move(stop)))
        };
    }
    std::shared_ptr<commands::CommandEntry> makePlaySceneCommand(
        commands::CommandEntry::Query query,
        cxx::move_only_function<commands::CommandResult<StartRunId>(commands::SessionTarget)> start
    )
    {
        return commands::CommandEntry::bind<kPlay>(
            contracts::CodeLease::builtin(), std::move(query),
            [start = std::move(start)](const commands::CommandInvocation& input) mutable
                -> commands::CommandResult<commands::DispatchReceipt> {
                auto result = start(std::get<commands::SessionTarget>(input.target()));
                if (!result)
                    return cxx::unexpected(result.error());
                return commands::DispatchReceipt{commands::AcceptedOperation{commands::OperationKindId{"run"}, result->serial}};
            }
        );
    }
}
