from pathlib import Path
import json,re
r=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
def edit(path,fn):
 p=r/path;s=p.read_text();p.write_text(fn(s))
def remove_namespace(s):
 start=s.find('\nnamespace\n{')
 if start>=0:
  end=s.index('\nnamespace lux::editor::application',start)
  s=s[:start]+s[end:]
 return s
def replace_command(s,old,new):
 a=s.index('        draft.commands.push_back('+old)
 b=s.index('\n        ));',a)+len('\n        ));')
 return s[:a]+new+s[b:]
def add_function(header,code):
 p=r/header;s=p.read_text();a=s.rfind('\n}')
 s=s[:a]+code+s[a:]
 s=s.replace('#pragma once','#pragma once\n#include <lux/engine/editor/desktop/ViewCommands.hpp>',1)
 p.write_text(s)

(r/'editor/workbench/desktop/include/lux/engine/editor/desktop/ViewCommands.hpp').write_text('''#pragma once
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/views/ViewFactory.hpp>

namespace lux::editor::desktop
{
    class ViewHost;
    using ToolOpening = cxx::move_only_function<commands::CommandResult<views::ViewId>(views::ViewTypeId)>;

    // Caller holds the catalog publication boundary. This operation only prepares complete views
    // and adopts them through the existing Host safe point; it does not own content or factories.
    [[nodiscard]] commands::CommandResult<views::ViewId> showTool(
        ViewHost&, const views::ViewFactorySnapshot&, object::ObjectDispatcherRef, views::ViewTypeId
    );
    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>> makeToolCommands(
        std::span<const std::shared_ptr<views::ViewFactoryEntry>>,
        commands::CommandEntry::Query, ToolOpening
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeCloseViewCommand(
        commands::CommandEntry::Query,
        cxx::move_only_function<commands::CommandResult<void>(views::ViewId)>
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeAnotherViewCommand(
        commands::CommandEntry::Query,
        cxx::move_only_function<commands::CommandResult<void>(commands::SessionTarget)>
    );
}
''')
(r/'editor/workbench/desktop/src/ViewCommands.cpp').write_text('''#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>

namespace lux::editor::desktop
{
    namespace
    {
        constexpr commands::CommandDescriptor kCloseView{
            commands::CommandIdView{"lux.editor.close-view"}, "Close View", "Window", "Ctrl+W",
            commands::ECommandScope::VIEW
        };
        constexpr commands::CommandDescriptor kAnotherView{
            commands::CommandIdView{"lux.editor.another-view"}, "Another View", "Window", "",
            commands::ECommandScope::SESSION
        };
    }
    commands::CommandResult<views::ViewId> showTool(
        ViewHost& host, const views::ViewFactorySnapshot& factories,
        object::ObjectDispatcherRef dispatcher, views::ViewTypeId type
    )
    {
        auto existing = host.describeAll();
        if (!existing)
            return workbench::detail::commandFailure(existing.error());
        for (const auto& view : *existing)
            if (view.type == type)
            {
                auto shown = host.show(view.id);
                if (!shown)
                    return workbench::detail::commandFailure(shown.error());
                auto focused = host.focus(view.id);
                if (!focused)
                    return workbench::detail::commandFailure(focused.error());
                return view.id;
            }
        views::ViewFactoryInput input{
            dispatcher, lux::ui::PaneId{type.name()}, contracts::CodeLease::builtin(),
            cxx::typeToken<std::monostate>(), std::make_shared<const std::monostate>()
        };
        auto candidate = factories.prepare(type, input);
        if (!candidate)
            return workbench::detail::commandFailure(candidate.error());
        auto adopted = host.adopt(*candidate, views::ViewRestoreKey{type.name()});
        if (!adopted)
            return workbench::detail::commandFailure(adopted.error());
        return adopted->id;
    }
    std::vector<std::shared_ptr<commands::CommandEntry>> makeToolCommands(
        std::span<const std::shared_ptr<views::ViewFactoryEntry>> views,
        commands::CommandEntry::Query query, ToolOpening open
    )
    {
        auto check = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        auto receiver = std::make_shared<ToolOpening>(std::move(open));
        std::vector<std::shared_ptr<commands::CommandEntry>> result;
        for (const auto& entry : views)
        {
            const auto& descriptor = entry->descriptor();
            if (descriptor.binding_type != cxx::typeToken<std::monostate>())
                continue;
            result.push_back(commands::CommandEntry::create(
                contracts::CodeLease::builtin(),
                {commands::CommandIdView{std::string("lux.editor.tool/") + std::string(descriptor.type.name())},
                 descriptor.label, "Window"},
                [check](const commands::CommandQuery& input) { return (*check)(input); },
                [receiver, type = views::ViewTypeId{descriptor.type.name()}](const commands::CommandInvocation&)
                    -> commands::CommandResult<commands::DispatchReceipt> {
                    auto shown = (*receiver)(type);
                    if (!shown)
                        return cxx::unexpected(shown.error());
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }
            ));
        }
        return result;
    }
    std::shared_ptr<commands::CommandEntry> makeCloseViewCommand(
        commands::CommandEntry::Query query,
        cxx::move_only_function<commands::CommandResult<void>(views::ViewId)> close
    )
    {
        return workbench::detail::bindCommand<kCloseView>(std::move(query),
            [close = std::move(close)](const commands::CommandInvocation& input) mutable {
                return close(std::get<views::ViewId>(input.target()));
            }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeAnotherViewCommand(
        commands::CommandEntry::Query query,
        cxx::move_only_function<commands::CommandResult<void>(commands::SessionTarget)> open
    )
    {
        return workbench::detail::bindCommand<kAnotherView>(std::move(query),
            [open = std::move(open)](const commands::CommandInvocation& input) mutable {
                return open(std::get<commands::SessionTarget>(input.target()));
            }
        );
    }
}
''')
(r/'editor/workbench/sinclude/lux/engine/editor/workbench/CommandSupport.hpp').write_text('''#pragma once
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>

namespace lux::editor::workbench::detail
{
    template <class Error> auto commandFailure(const Error& error)
    {
        auto detail = viewFailure(error);
        return cxx::unexpected(commands::CommandFailure{
            detail.code == views::EViewFactoryError::BUSY ? commands::ECommandError::BUSY
                                                        : commands::ECommandError::DOMAIN_FAILURE,
            std::move(detail.domain), detail.domain_code, std::move(detail.detail)
        });
    }
    template <const commands::CommandDescriptor& Descriptor, class Action>
    std::shared_ptr<commands::CommandEntry> bindCommand(commands::CommandEntry::Query query, Action action)
    {
        return commands::CommandEntry::bind<Descriptor>(contracts::CodeLease::builtin(), std::move(query),
            [action = std::move(action)](const commands::CommandInvocation& input) mutable
                -> commands::CommandResult<commands::DispatchReceipt> {
                auto result = action(input);
                if (!result)
                    return cxx::unexpected(result.error());
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        );
    }
    template <const commands::CommandDescriptor& Descriptor, const views::ViewFactoryDescriptor& Factory>
    std::shared_ptr<commands::CommandEntry> bindToolCommand(
        commands::CommandEntry::Query query, desktop::ToolOpening open
    )
    {
        return bindCommand<Descriptor>(std::move(query),
            [open = std::move(open)](const commands::CommandInvocation&) mutable {
                return open(views::ViewTypeId{Factory.type.name()});
            }
        );
    }
}
''')

tools=[
 ('tasks','TaskView','tasks','makeTasksCommand','lux.editor.tasks','Background Tasks','Window'),
 ('project','ProjectView','project','makeAssetsCommand','lux.editor.assets','Assets','Window'),
 ('project/tools','ImportView','project','makeImportCommand','lux.editor.import','Import Assets','File'),
 ('project/tools','RecentProjectsView','project','makeRecentProjectsCommand','lux.editor.project.recent','Recent Projects','File'),
 ('project/tools','ResultsView','project','makeResultsCommand','lux.editor.content.results','Content and Operations','Window'),
 ('project/tools','WorkspaceView','project','makeWorkspaceCommand','lux.editor.workspace','Workspace','Window')]
for directory,stem,ns,fn,id,label,group in tools:
 header=f'editor/workbench/{directory}/include/lux/engine/editor/{ns}/{stem}.hpp'
 add_function(header,f'''\n    [[nodiscard]] std::shared_ptr<commands::CommandEntry> {fn}(
        commands::CommandEntry::Query, desktop::ToolOpening
    );\n''')
 p=r/f'editor/workbench/{directory}/src/{stem}.cpp';s=p.read_text()
 s='#include <lux/engine/editor/workbench/CommandSupport.hpp>\n'+s
 a=s.index('        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{')
 s=s[:a]+f'''        constexpr commands::CommandDescriptor kCommand{{
            commands::CommandIdView{{"{id}"}}, "{label}", "{group}"
        }};
'''+s[a:]
 a=s.rfind('\n}')
 s=s[:a]+f'''
    std::shared_ptr<commands::CommandEntry> {fn}(
        commands::CommandEntry::Query query, desktop::ToolOpening open
    )
    {{
        return workbench::detail::bindToolCommand<kCommand, kFactoryDescriptor>(std::move(query), std::move(open));
    }}
'''+s[a:];p.write_text(s)

app='editor/application/'
p=r/(app+'src/EditorContent.cpp');s=remove_namespace(p.read_text())
a=s.index('        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_close_view>')
b=s.index('        auto creation_available =',a)
s=s[:a]+'''        draft.commands.push_back(desktop::makeCloseViewCommand(running,
            [this](views::ViewId id) -> commands::CommandResult<void> {
                auto result = closeView(id);
                if (!result)
                    return cxx::unexpected(commands::CommandFailure{commands::ECommandError::DOMAIN_FAILURE,
                        result.error().domain, result.error().reason, result.error().message});
                return {};
            }
        ));
        draft.commands.push_back(desktop::makeAnotherViewCommand(running,
            [this](commands::SessionTarget target) -> commands::CommandResult<void> {
                auto result = makeContentView({{target.id}, target.id}, true, contributions_.snapshot());
                if (!result)
                    return cxx::unexpected(commands::CommandFailure{commands::ECommandError::DOMAIN_FAILURE,
                        result.error().domain, result.error().reason, result.error().message});
                return {};
            }
        ));
        draft.commands.push_back(project::makeAssetsCommand(running, toolOpening()));
        draft.commands.push_back(tasks::makeTasksCommand(running, toolOpening()));
'''+s[b:]
s='#include <lux/engine/editor/project/ProjectView.hpp>\n#include <lux/engine/editor/tasks/TaskView.hpp>\n'+s;p.write_text(s)
for file,old,fn in [('EditorResults.cpp','command_lux_editor_content_results','makeResultsCommand'),
                    ('EditorWorkspace.cpp','command_lux_editor_workspace','makeWorkspaceCommand')]:
 p=r/(app+'src/'+file);s=remove_namespace(p.read_text())
 new='''        draft.commands.push_back(project::'''+fn+'''(
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            }, toolOpening()
        ));'''
 s=replace_command(s,'commands::CommandEntry::bind<'+old+'>',new);p.write_text(s)
p=r/(app+'src/EditorProjectTools.cpp');s=p.read_text()
for old in ('command_lux_editor_import','command_lux_editor_project_recent'):
 a=s.index('    constexpr lux::editor::commands::CommandDescriptor '+old)
 b=s.index('\n    };',a)+len('\n    };');s=s[:a]+s[b:]
 fn='makeImportCommand' if old.endswith('import') else 'makeRecentProjectsCommand'
 s=replace_command(s,'commands::CommandEntry::bind<'+old+'>','''        draft.commands.push_back(project::'''+fn+'''(
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            }, toolOpening()
        ));''')
p.write_text(s)
p=r/(app+'src/EditorCommands.cpp');s=p.read_text()
a=s.index('            for (const auto& entry : contributed->views)')
b=s.index('            append(draft.code, contributed->code);',a)
s=s[:a]+'''            auto tools = desktop::makeToolCommands(contributed->views,
                [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                    return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
                }, toolOpening()
            );
            append(draft.commands, tools);
'''+s[b:];p.write_text(s)
p=r/(app+'src/EditorViewClosure.cpp');s=p.read_text()
a=s.index('    EditorResult<views::ViewId> EditorApplication::Impl::showTool(')
s=s[:a]+'''    desktop::ToolOpening EditorApplication::Impl::toolOpening()
    {
        return [host = &desktop_->views(), catalog = &contributions_, dispatcher = messages_.dispatcherRef()]
            (views::ViewTypeId type) -> commands::CommandResult<views::ViewId> {
            // Executed inside the original CommandRegistry dispatch, which excludes contribution
            // publication. Pin the current catalog without attempting a recursive read batch.
            return desktop::showTool(*host, catalog->snapshot().views(), dispatcher, std::move(type));
        };
    }
    EditorResult<views::ViewId> EditorApplication::Impl::showTool(views::ViewTypeId type)
    {
        auto result = desktop::showTool(
            desktop_->views(), contributions_.snapshot().views(), messages_.dispatcherRef(), std::move(type)
        );
        if (!result)
            return applicationFailure("tool.show", result.error());
        return *result;
    }
}
''';p.write_text(s)
edit(app+'pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp',lambda s:s.replace(
 '#pragma once','#pragma once\n#include <lux/engine/editor/desktop/ViewCommands.hpp>',1).replace(
 '        [[nodiscard]] EditorResult<views::ViewId> showTool(views::ViewTypeId);',
 '        [[nodiscard]] EditorResult<views::ViewId> showTool(views::ViewTypeId);\n        [[nodiscard]] desktop::ToolOpening toolOpening();'))

# Actual static closure and installed header ownership.
p=r/'editor/workbench/desktop/CMakeLists.txt';s=p.read_text().replace('src/ViewHost.cpp src/ViewFactory.cpp','src/ViewCommands.cpp src/ViewHost.cpp src/ViewFactory.cpp')
s=s.replace('target_link_libraries(view_host PUBLIC ', 'target_include_directories(view_host PRIVATE ${PROJECT_SOURCE_DIR}/editor/workbench/sinclude)\ntarget_link_libraries(view_host PUBLIC lux::engine::editor::editor_commands ')
s=s.replace('component_add_transitive_commands(view_host','component_add_transitive_commands(view_host\n    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands)"')
s=s.replace('install(FILES\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WorkspaceActions.hpp',
 'install(FILES\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/ViewCommands.hpp\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WorkspaceActions.hpp');p.write_text(s)
for directory in ('tasks','project','project/tools'):
 p=r/f'editor/workbench/{directory}/CMakeLists.txt';s=p.read_text()
 s=s.replace('PRIVATE lux::engine::editor::view_host imgui::core','lux::engine::editor::view_host PRIVATE imgui::core');p.write_text(s)
p=r/'editor/tests/architecture/rules.json';x=json.loads(p.read_text());l=x['editor_layering']
l['files']['editor/workbench/desktop/src/ViewCommands.cpp']=['view_host']
l['files']['editor/workbench/desktop/include/lux/engine/editor/desktop/ViewCommands.hpp']=['view_host']
l['files']['editor/workbench/sinclude/lux/engine/editor/workbench/CommandSupport.hpp']=['view_host','project_ui','project_tools_ui','tasks_ui']
for section in x.values():
 if not isinstance(section,dict):continue
 closure=section.get('closure',{})
 if 'view_host' in closure:
  closure['editor_commands']={'path':'editor/activities/commands'}
  closure['edit_sessions']={'path':'editor/editing/sessions'}
  closure['edit_history']={'path':'editor/editing/history'}
  closure['ui_input']={'path':'modules/function/ui'}
 if section is x.get('view_host'):
  section['direct'].append('editor_commands')
 if section is x.get('project_ui'):
  # Direct provider did not change; its public visibility now matches the public command signature.
  pass
 for name in ('lux/engine/editor/desktop/ViewCommands.hpp','lux/engine/editor/workbench/CommandSupport.hpp'):
  if isinstance(section.get('headers'),list) and 'view_host' in closure and name not in section['headers']:
   section['headers'].append(name)
p.write_text(json.dumps(x,indent=2)+'\n')
print('Applied concrete tool commands; inspect diff and build the responsibility closure.')
