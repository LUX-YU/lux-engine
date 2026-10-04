from pathlib import Path
import json
r=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
def add(path,decl):
 p=r/path;s=p.read_text();i=s.rfind('\n}');s=s[:i]+decl+s[i:];p.write_text(s)
def command_statement(s,name):
 a=s.index('        draft.commands.push_back(commands::CommandEntry::bind<'+name+'>')
 b=s.index('\n        ));',a)+len('\n        ));')
 return a,b
def cut_namespace(s):
 a=s.index('\nnamespace\n{');b=s.index('\nnamespace lux::editor::application',a);return s[:a]+s[b:]

# Scene creation now binds its own exact window type rather than making Application repeat it.
p=r/'editor/workbench/scene/include/lux/engine/editor/scene/SceneCreationView.hpp';s=p.read_text().replace('#pragma once','#pragma once\n#include <lux/engine/editor/desktop/ViewCommands.hpp>',1).replace('cxx::move_only_function<commands::CommandResult<void>()> show','desktop::ToolOpening');p.write_text(s)
p=r/'editor/workbench/scene/src/SceneCreationView.cpp';s=p.read_text();a=s.index('    std::shared_ptr<commands::CommandEntry> makeNewSceneCommand(')
s='#include <lux/engine/editor/workbench/CommandSupport.hpp>\n'+s[:a]+'''    std::shared_ptr<commands::CommandEntry> makeNewSceneCommand(
        commands::CommandEntry::Query query, desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kNewCommand, kCreationDescriptor>(std::move(query), std::move(open));
    }
}
''';p.write_text(s)
p=r/'editor/application/src/EditorSceneTools.cpp';s=p.read_text();a=s.index('        draft.commands.push_back(scene::makeNewSceneCommand(');b=s.index('\n        ));',a)+len('\n        ));');s=s[:a]+'        draft.commands.push_back(scene::makeNewSceneCommand(available, toolOpening()));'+s[b:];p.write_text(s)

add('editor/workbench/project/tools/include/lux/engine/editor/project/ProjectCreationView.hpp','''
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeProjectCreationCommand(
        commands::CommandEntry::Query, desktop::ToolOpening,
        cxx::move_only_function<commands::CommandResult<void>()> start
    );
''')
p=r/'editor/workbench/project/tools/include/lux/engine/editor/project/ProjectCreationView.hpp';s=p.read_text().replace('#pragma once','#pragma once\n#include <lux/engine/editor/desktop/ViewCommands.hpp>',1);p.write_text(s)
p=r/'editor/workbench/project/tools/src/ProjectCreationView.cpp';s=p.read_text();s='#include <lux/engine/editor/workbench/CommandSupport.hpp>\n'+s
a=s.index('        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{');s=s[:a]+'''        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.project.create"}, "New Project", "File"
        };
'''+s[a:];a=s.rfind('\n}');s=s[:a]+'''
    std::shared_ptr<commands::CommandEntry> makeProjectCreationCommand(
        commands::CommandEntry::Query query, desktop::ToolOpening open,
        cxx::move_only_function<commands::CommandResult<void>()> start
    )
    {
        return workbench::detail::bindCommand<kCommand>(std::move(query),
            [open = std::move(open), start = std::move(start)](const commands::CommandInvocation&) mutable
                -> commands::CommandResult<void> {
                auto shown = open(views::ViewTypeId{kFactoryDescriptor.type.name()});
                if (!shown)
                    return cxx::unexpected(shown.error());
                // Construction and Host adoption precede work which needs the new view's maintenance.
                return start();
            }
        );
    }
'''+s[a:];p.write_text(s)
p=r/'editor/application/src/EditorProjectCreation.cpp';s=cut_namespace(p.read_text());a,b=command_statement(s,'command_lux_editor_project_create')
s=s[:a]+'''        draft.commands.push_back(project::makeProjectCreationCommand(
            [phase = &phase_](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{*phase == EApplicationPhase::RUNNING};
            }, toolOpening(),
            [creation = &project_creation_]() -> commands::CommandResult<void> {
                auto started = (*creation)->start();
                if (!started && started.error().code != EEditorError::BUSY)
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE, started.error().domain,
                        started.error().reason, started.error().message
                    });
                return {};
            }
        ));'''+s[b:];p.write_text(s)
# Remove the now unused same-purpose App wrapper in the same migration.
p=r/'editor/application/src/EditorViewClosure.cpp';s=p.read_text();a=s.index('    EditorResult<views::ViewId> EditorApplication::Impl::showTool(');s=s[:a]+'}\n';p.write_text(s)
p=r/'editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp';s=p.read_text().replace('        [[nodiscard]] EditorResult<views::ViewId> showTool(views::ViewTypeId);\n','');p.write_text(s)

# Recovery commands bind actual typed intents in their Workspace provider.
add('editor/workbench/project/tools/include/lux/engine/editor/project/WorkspaceView.hpp','''
    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>> makeRecoveryCommands(
        commands::CommandEntry::Query, WorkspaceView::Request
    );
''')
p=r/'editor/workbench/project/tools/src/WorkspaceView.cpp';s=p.read_text();a=s.index('        constexpr commands::CommandDescriptor kCommand{')
s=s[:a]+'''        constexpr commands::CommandDescriptor kCaptureRecovery{
            commands::CommandIdView{"lux.editor.recovery.capture"}, "Record content locations", "Workspace"
        };
        constexpr commands::CommandDescriptor kRestoreRecovery{
            commands::CommandIdView{"lux.editor.recovery.restore"}, "Restore recorded content", "Workspace"
        };
'''+s[a:];a=s.rfind('\n}');s=s[:a]+'''
    std::vector<std::shared_ptr<commands::CommandEntry>> makeRecoveryCommands(
        commands::CommandEntry::Query query, WorkspaceView::Request request
    )
    {
        auto check = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        auto receiver = std::make_shared<WorkspaceView::Request>(std::move(request));
        const auto bind = [&]<const commands::CommandDescriptor& Descriptor>(VWorkspaceIntent intent) {
            return workbench::detail::bindCommand<Descriptor>(
                [check](const commands::CommandQuery& input) { return (*check)(input); },
                [receiver, intent = std::move(intent)](const commands::CommandInvocation&)
                    -> commands::CommandResult<void> {
                    auto result = (*receiver)(intent);
                    if (!result)
                        return workbench::detail::commandFailure(result.error());
                    return {};
                }
            );
        };
        return {bind.template operator()<kCaptureRecovery>(CaptureRecovery{}),
                bind.template operator()<kRestoreRecovery>(RestoreRecovery{})};
    }
'''+s[a:];p.write_text(s)
p=r/'editor/application/src/EditorWorkspace.cpp';s=p.read_text();a=s.index('        // The same recovery operations');b=s.index('        draft.views.push_back(project::makeWorkspaceViewFactory(',a)
s=s[:a]+'''        auto recovery = project::makeRecoveryCommands(
            [phase = &phase_](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{*phase == EApplicationPhase::RUNNING};
            }, [this](VWorkspaceIntent intent) { return executeWorkspaceIntent(intent); }
        );
        draft.commands.insert(draft.commands.end(),
            std::make_move_iterator(recovery.begin()), std::make_move_iterator(recovery.end()));
'''+s[b:];p.write_text(s)

# Initial asset resolution stays in the existing project activity, including source identity validation.
p=r/'editor/activities/project/include/lux/engine/editor/storage/ProjectContentOpening.hpp';s=p.read_text();a=s.rfind('\n}');s=s[:a]+'''
    [[nodiscard]] EditorResult<AssetReference> initialSceneReference(const ProjectStorage&);
'''+s[a:];p.write_text(s)
p=r/'editor/activities/project/src/ProjectAssetSource.cpp';s=p.read_text();a=s.rfind('\n}');s=s[:a]+'''
    EditorResult<AssetReference> initialSceneReference(const ProjectStorage& project)
    {
        const auto& manifest = project.manifest();
        const auto found = std::ranges::find(manifest.assets, manifest.default_scene, &ProjectAssetEntry::source_path);
        if (found == manifest.assets.end())
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "initial-scene.missing"});
        return project.catalogModel().reference(found->id);
    }
'''+s[a:];p.write_text(s)
# Both project navigation commands belong to the project browsing view theme.
add('editor/workbench/project/tools/include/lux/engine/editor/project/RecentProjectsView.hpp','''
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeOpenProjectCommand(
        commands::CommandEntry::Query, cxx::move_only_function<commands::CommandResult<void>()>
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeInitialSceneCommand(
        commands::CommandEntry::Query, ProjectStorage&,
        cxx::move_only_function<commands::CommandResult<void>(AssetReference)>
    );
''')
p=r/'editor/workbench/project/tools/include/lux/engine/editor/project/RecentProjectsView.hpp';s=p.read_text().replace('    class RecentProjects;', '    class RecentProjects;\n    class ProjectStorage;\n    struct AssetReference;');p.write_text(s)
p=r/'editor/workbench/project/tools/src/RecentProjectsView.cpp';s=p.read_text();s='#include <lux/engine/editor/storage/ProjectContentOpening.hpp>\n'+s
a=s.index('        constexpr commands::CommandDescriptor kCommand{');s=s[:a]+'''        constexpr commands::CommandDescriptor kOpenProject{
            commands::CommandIdView{"lux.editor.project.open"}, "Open Project in New Editor", "File"
        };
        constexpr commands::CommandDescriptor kInitialScene{
            commands::CommandIdView{"lux.editor.initial-scene"}, "Open Initial Scene", "File"
        };
'''+s[a:];a=s.rfind('\n}');s=s[:a]+'''
    std::shared_ptr<commands::CommandEntry> makeOpenProjectCommand(
        commands::CommandEntry::Query query, cxx::move_only_function<commands::CommandResult<void>()> request
    )
    {
        return workbench::detail::bindCommand<kOpenProject>(std::move(query),
            [request = std::move(request)](const commands::CommandInvocation&) mutable { return request(); }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeInitialSceneCommand(
        commands::CommandEntry::Query query, ProjectStorage& project,
        cxx::move_only_function<commands::CommandResult<void>(AssetReference)> open
    )
    {
        return workbench::detail::bindCommand<kInitialScene>(
            [query = std::move(query), &project](const commands::CommandQuery& input) mutable
                -> commands::CommandResult<commands::CommandState> {
                auto state = query(input);
                if (state)
                    state->enabled = state->enabled && !project.manifest().default_scene.empty();
                return state;
            }, [&project, open = std::move(open)](const commands::CommandInvocation&) mutable
                -> commands::CommandResult<void> {
                auto reference = initialSceneReference(project);
                if (!reference)
                    return workbench::detail::commandFailure(reference.error());
                return open(*reference);
            }
        );
    }
'''+s[a:];p.write_text(s)
p=r/'editor/application/src/EditorProjectTools.cpp';s=cut_namespace(p.read_text())
a,b=command_statement(s,'command_lux_editor_initial_scene');s=s[:a]+'''        draft.commands.push_back(project::makeInitialSceneCommand(
            [phase = &phase_](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{*phase == EApplicationPhase::RUNNING};
            }, *project_, [intents = &open_intents_](AssetReference reference) -> commands::CommandResult<void> {
                if (intents->size() == 64)
                    return cxx::unexpected(commands::CommandFailure{commands::ECommandError::CAPACITY, "initial-scene.queue"});
                intents->push_back(reference);
                return {};
            }
        ));'''+s[b:]
a,b=command_statement(s,'command_lux_editor_project_open');part=s[a:b]
part=part.replace('commands::CommandEntry::bind<command_lux_editor_project_open>(\n            contracts::CodeLease::builtin(),','project::makeOpenProjectCommand(')
part=part.replace('[this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt>', '[this]() -> commands::CommandResult<void>')
part=part.replace('return commands::DispatchReceipt{commands::ImmediateCompletion{}};', 'return {};');s=s[:a]+part+s[b:]
# About is product metadata and deliberately remains in Application, now a literal declaration.
s=s.replace('namespace lux::editor::application','''namespace
{
    constexpr lux::editor::commands::CommandDescriptor kAbout{
        lux::editor::commands::CommandIdView{"lux.editor.about"}, "Lux Editor " LUX_EDITOR_VERSION, "Help"
    };
}
namespace lux::editor::application''',1)
s=s.replace('commands::CommandEntry::create(\n            contracts::CodeLease::builtin(),\n            commands::CommandDescriptor{\n                commands::CommandIdView{"lux.editor.about"},\n                "Lux Editor " LUX_EDITOR_VERSION,\n                "Help"\n            },', 'commands::CommandEntry::bind<kAbout>(\n            contracts::CodeLease::builtin(),');p.write_text(s)

p=r/'editor/tests/architecture/rules.json';x=json.loads(p.read_text());l=x['editor_layering']
l['shared_headers']['editor/workbench/sinclude/lux/engine/editor/workbench/CommandSupport.hpp']['consumers'].append('scene_ui')
l['files']['editor/workbench/sinclude/lux/engine/editor/workbench/CommandSupport.hpp'].append('scene_ui')
if 'lux/engine/editor/storage/ProjectContentOpening.hpp' not in x['scene_ui']['headers']:pass
p.write_text(json.dumps(x,indent=2)+'\n')
print('Applied remaining fixed declarations; inspect API and behavior before building.')
