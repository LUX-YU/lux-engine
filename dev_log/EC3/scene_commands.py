from pathlib import Path
r = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p = r/'editor/application/src/EditorSceneTools.cpp'
s = p.read_text()
start=s.index('namespace\n{')
end=s.index('namespace lux::editor::application', start)
s=s[:start]+s[end:]
s=s.replace('showSceneTool(views::ViewId source, std::string_view role)', 'showSceneTool(views::ViewId source, scene::ESceneTool kind)')
start=s.index('        const auto kind = role ==')
end=s.index('        auto source_group',start)
s=s[:start]+s[end:]
s=s.replace('const auto name = std::string(role) + "-" + std::to_string(next_view_++);', 'const auto name = "scene-tool-" + std::to_string(next_view_++);')
start=s.index('    void EditorApplication::Impl::installSceneCommands')
s=s[:start]+'''    EditorResult<void> EditorApplication::Impl::stepRun(scene::RunId run)
    {
        auto owner = std::ranges::find(run_presentations_, std::optional{run}, &RunPresentation::run);
        const bool is_full = owner == run_presentations_.end() || owner->steps.size() >= 64;
        if (is_full)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.steps"});
        auto step = runs_.step(run);
        if (!step)
            return applicationFailure("run.step", step.error());
        owner->steps.push_back(*step);
        return {};
    }
    void EditorApplication::Impl::installSceneCommands(extensions::ContributionDraft& draft)
    {
        const auto available = [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
            return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
        };
        draft.views.push_back(scene::makeSceneCreationViewFactory(sceneConfigurationInputs(), contentCreation()));
        draft.commands.push_back(scene::makeNewSceneCommand(
            available, [this]() -> commands::CommandResult<void> {
                auto shown = showTool(views::ViewTypeId{"lux.editor.scene.creation"});
                if (!shown)
                    return cxx::unexpected(commandFailure(shown.error()));
                return {};
            }
        ));
        draft.commands.push_back(scene::makePlaySceneCommand(
            available, [this](commands::SessionTarget target) -> commands::CommandResult<scene::StartRunId> {
                auto started = play(target);
                if (!started)
                    return cxx::unexpected(commandFailure(started.error()));
                return *started;
            }
        ));
        auto tools = scene::makeSceneToolCommands(
            available, [this](views::ViewId view, scene::ESceneTool kind) -> commands::CommandResult<void> {
                auto shown = showSceneTool(view, kind);
                if (!shown)
                    return cxx::unexpected(commandFailure(shown.error()));
                return {};
            }
        );
        auto runs = scene::makeRunViewCommands(
            available, desktop_->views(), runs_,
            [this](scene::RunId id) -> commands::CommandResult<void> {
                auto result = stepRun(id);
                if (!result)
                {
                    if (result.error().code == EEditorError::BUSY)
                        return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "run.steps"});
                    return cxx::unexpected(commandFailure(result.error()));
                }
                return {};
            },
            [this](scene::RunId id) -> commands::CommandResult<void> {
                auto result = stopRun(id);
                if (!result)
                    return cxx::unexpected(commandFailure(result.error()));
                return {};
            }
        );
        draft.commands.insert(draft.commands.end(), tools.begin(), tools.end());
        draft.commands.insert(draft.commands.end(), runs.begin(), runs.end());
    }
}
'''
p.write_text(s)
p=r/'editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp'
s=p.read_text().replace('#include <lux/engine/editor/scene/SceneView.hpp>', '#include <lux/engine/editor/scene/SceneView.hpp>\n#include <lux/engine/editor/scene/SceneTools.hpp>')
s=s.replace('EditorResult<views::ViewId> showSceneTool(views::ViewId, std::string_view);', 'EditorResult<views::ViewId> showSceneTool(views::ViewId, scene::ESceneTool);\n        [[nodiscard]] EditorResult<void> stepRun(scene::RunId);')
p.write_text(s)
p=r/'editor/workbench/scene/CMakeLists.txt'
s=p.read_text().replace('src/SceneTools.cpp ', 'src/SceneTools.cpp src/SceneCommands.cpp ')
p.write_text(s)
