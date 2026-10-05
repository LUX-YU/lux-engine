#include <lux/engine/editor/scene/RunStore.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>

namespace lux::editor::scene
{
    namespace
    {
        constexpr commands::CommandDescriptor kOutliner{
            .id = commands::CommandIdView{"lux.editor.scene.outliner"},
            .label = "outliner",
            .group = "Scene",
            .shortcut = "",
            .scope = commands::ECommandScope::VIEW,
            .target_type = cxx::typeToken<lux::ui::PaneHandle>()
        };
        constexpr commands::CommandDescriptor kInspector{
            .id = commands::CommandIdView{"lux.editor.scene.inspector"},
            .label = "inspector",
            .group = "Scene",
            .shortcut = "",
            .scope = commands::ECommandScope::VIEW,
            .target_type = cxx::typeToken<lux::ui::PaneHandle>()
        };
        constexpr commands::CommandDescriptor kResources{
            .id = commands::CommandIdView{"lux.editor.scene.resources"},
            .label = "resources",
            .group = "Scene",
            .shortcut = "",
            .scope = commands::ECommandScope::VIEW,
            .target_type = cxx::typeToken<lux::ui::PaneHandle>()
        };
        constexpr commands::CommandDescriptor kConfiguration{
            .id = commands::CommandIdView{"lux.editor.scene.configuration"},
            .label = "configuration",
            .group = "Scene",
            .shortcut = "",
            .scope = commands::ECommandScope::VIEW,
            .target_type = cxx::typeToken<lux::ui::PaneHandle>()
        };
        constexpr commands::CommandDescriptor kPause{
            .id = commands::CommandIdView{"lux.editor.scene.pause"},
            .label = "pause",
            .group = "Scene",
            .shortcut = "",
            .scope = commands::ECommandScope::VIEW,
            .target_type = cxx::typeToken<lux::ui::PaneHandle>()
        };
        constexpr commands::CommandDescriptor kResume{
            .id = commands::CommandIdView{"lux.editor.scene.resume"},
            .label = "resume",
            .group = "Scene",
            .shortcut = "",
            .scope = commands::ECommandScope::VIEW,
            .target_type = cxx::typeToken<lux::ui::PaneHandle>()
        };
        constexpr commands::CommandDescriptor kStep{
            .id = commands::CommandIdView{"lux.editor.scene.step"},
            .label = "step",
            .group = "Scene",
            .scope = commands::ECommandScope::VIEW,
            .target_type = cxx::typeToken<lux::ui::PaneHandle>()
        };
        constexpr commands::CommandDescriptor kStop{
            .id = commands::CommandIdView{"lux.editor.scene.stop"},
            .label = "stop",
            .group = "Scene",
            .scope = commands::ECommandScope::VIEW,
            .target_type = cxx::typeToken<lux::ui::PaneHandle>()
        };
        constexpr commands::CommandDescriptor kPlay{
            commands::CommandIdView{"lux.editor.play"},
            "Play Frozen Scene",
            "Scene",
            "Ctrl+P",
            commands::ECommandScope::SESSION
        };

        template <const commands::CommandDescriptor& Descriptor, class Action>
        std::shared_ptr<commands::CommandEntry> bindViewCommand(
            std::shared_ptr<commands::CommandEntry::Query> query,
            Action action
        )
        {
            return commands::CommandEntry::bind<Descriptor>(
                lux::object::CodeLease::builtin(),
                [query](const commands::CommandQuery& input) { return (*query)(input); },
                [action = std::move(action)](const commands::CommandInvocation& input
                ) mutable -> commands::CommandResult<commands::DispatchReceipt>
                {
                    auto result = action(*input.view<lux::ui::PaneHandle>());
                    if (!result)
                    {
                        return cxx::unexpected(result.error());
                    }
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }
            );
        }
        template <class Action> auto forRun(lux::ui::Root& root, Action action)
        {
            return
                [&root, action = std::move(action)](lux::ui::PaneHandle view) mutable -> commands::CommandResult<void>
            {
                auto group = shareSceneInteraction(root, view);
                if (!group)
                {
                    return workbench::detail::commandFailure(group.error());
                }
                const auto run = (*group)->run();
                if (!run)
                {
                    return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET, "run.view"});
                }
                return action(*run);
            };
        }
    } // namespace
    std::vector<std::shared_ptr<commands::CommandEntry>> makeSceneToolCommands(
        commands::CommandEntry::Query query,
        cxx::move_only_function<commands::CommandResult<void>(lux::ui::PaneHandle, ESceneTool)> receiver
    )
    {
        auto check = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        auto show = std::make_shared<decltype(receiver)>(std::move(receiver));
        return {
            bindViewCommand<kOutliner>(
                check,
                [show](lux::ui::PaneHandle id) { return (*show)(id, ESceneTool::OUTLINER); }
            ),
            bindViewCommand<kInspector>(
                check,
                [show](lux::ui::PaneHandle id) { return (*show)(id, ESceneTool::INSPECTOR); }
            ),
            bindViewCommand<kResources>(
                check,
                [show](lux::ui::PaneHandle id) { return (*show)(id, ESceneTool::RESOURCES); }
            ),
            bindViewCommand<kConfiguration>(
                check,
                [show](lux::ui::PaneHandle id) { return (*show)(id, ESceneTool::CONFIGURATION); }
            )
        };
    }
    std::vector<std::shared_ptr<commands::CommandEntry>> makeRunViewCommands(
        commands::CommandEntry::Query query,
        lux::ui::Root& root,
        RunStore& runs,
        cxx::move_only_function<commands::CommandResult<void>(RunId)> step,
        cxx::move_only_function<commands::CommandResult<void>(RunId)> stop
    )
    {
        auto check = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        return {
            bindViewCommand<kPause>(
                check,
                forRun(
                    root,
                    [&runs](RunId id) -> commands::CommandResult<void>
                    {
                        auto result = runs.pause(id);
                        if (!result)
                        {
                            return workbench::detail::commandFailure(result.error());
                        }
                        return {};
                    }
                )
            ),
            bindViewCommand<kResume>(
                check,
                forRun(
                    root,
                    [&runs](RunId id) -> commands::CommandResult<void>
                    {
                        auto result = runs.resume(id);
                        if (!result)
                        {
                            return workbench::detail::commandFailure(result.error());
                        }
                        return {};
                    }
                )
            ),
            bindViewCommand<kStep>(check, forRun(root, std::move(step))),
            bindViewCommand<kStop>(check, forRun(root, std::move(stop)))
        };
    }
    std::shared_ptr<commands::CommandEntry> makePlaySceneCommand(
        commands::CommandEntry::Query query,
        cxx::move_only_function<commands::CommandResult<StartRunId>(commands::SessionTarget)> start
    )
    {
        return commands::CommandEntry::bind<kPlay>(
            lux::object::CodeLease::builtin(),
            std::move(query),
            [start = std::move(start)](const commands::CommandInvocation& input
            ) mutable -> commands::CommandResult<commands::DispatchReceipt>
            {
                auto result = start(std::get<commands::SessionTarget>(input.target()));
                if (!result)
                {
                    return cxx::unexpected(result.error());
                }
                return commands::DispatchReceipt{
                    commands::AcceptedOperation{commands::OperationKindId{"run"}, result->serial}
                };
            }
        );
    }
} // namespace lux::editor::scene
