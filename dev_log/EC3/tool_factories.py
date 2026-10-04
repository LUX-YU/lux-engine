from pathlib import Path
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
def add(folder, stem, ns, key, label, signature, body):
 cpp=s/f'editor/workbench/{folder}/src/{stem}.cpp';text=cpp.read_text();text='#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>\n'+text
 text+=f'\nnamespace lux::editor::{ns}\n{{\n    namespace\n    {{\n        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{{\n            views::ViewTypeIdView{{"{key}"}}, "{label}", cxx::typeToken<std::monostate>()\n        }};\n    }}\n'+signature+'\n    {\n'+body+'\n    }\n}\n';cpp.write_text(text)
 h=s/f'editor/workbench/{folder}/include/lux/engine/editor/{ns}/{stem}.hpp';text=h.read_text().replace('#pragma once','#pragma once\n#include <lux/cxx/core/move_only_function.hpp>',1);text=text.replace('namespace lux::editor::'+ns+'\n{','namespace lux::editor::views { class ViewFactoryEntry; }\n\nnamespace lux::editor::'+ns+'\n{',1);i=text.rfind('}');text=text[:i]+signature.replace('    std::shared_ptr','    [[nodiscard]] std::shared_ptr')+';\n'+text[i:];h.write_text(text)
add('tasks','TaskView','tasks','lux.editor.tasks','Tasks','    std::shared_ptr<views::ViewFactoryEntry> makeTaskViewFactory(TaskMonitor& monitor)', '''        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(contracts::CodeLease::builtin(),
            [&monitor](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                return makeTaskView(input.dispatcher(), input.paneId(), monitor);
            }
        );''')
add('project','ProjectView','project','lux.editor.project','Assets','''    std::shared_ptr<views::ViewFactoryEntry> makeProjectViewFactory(
        ProjectCatalogModel& catalog, cxx::move_only_function<void(const AssetReference&)> open
    )''','''        auto receiver = std::make_shared<cxx::move_only_function<void(const AssetReference&)>>(std::move(open));
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(contracts::CodeLease::builtin(),
            [&catalog, receiver](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                auto view = makeProjectView(input.dispatcher(), input.paneId(), catalog);
                auto connected = workbench::detail::connectIntent(view, &ProjectView::openRequested, receiver);
                if (!connected)
                    return cxx::unexpected(connected.error());
                return view;
            }
        );''')
add('project/tools','RecentProjectsView','project','lux.editor.recent-projects','Recent Projects','''    std::shared_ptr<views::ViewFactoryEntry> makeRecentProjectsViewFactory(
        RecentProjects& recent, cxx::move_only_function<EditorResult<void>(const std::filesystem::path&)> open
    )''','''        auto receiver = std::make_shared<cxx::move_only_function<EditorResult<void>(const std::filesystem::path&)>>(
            std::move(open)
        );
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(contracts::CodeLease::builtin(),
            [&recent, receiver](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                auto pane = std::make_unique<RecentProjectsView>(input.dispatcher(), input.paneId(), recent);
                auto connection = object::LuxObject::connect(pane.get(), &RecentProjectsView::openRequested,
                    [target = pane.get(), receiver](const std::filesystem::path& path) noexcept {
                        auto result = (*receiver)(path);
                        if (!result)
                            target->showFailure(std::move(result.error()));
                    }
                );
                if (!connection)
                    return cxx::unexpected(workbench::detail::viewFailure(connection.error()));
                views::DetachedView view{contracts::CodeLease::builtin(), std::move(pane)};
                view.addConnection(std::move(*connection));
                return view;
            }
        );''')
add('project/tools','ImportView','project','lux.editor.import','Import Assets','''    std::shared_ptr<views::ViewFactoryEntry> makeImportViewFactory(
        ProjectCatalogModel& catalog, assets::ModelImporter& importer,
        cxx::move_only_function<void(lux::ui::PaneId)> browse
    )''','''        auto receiver = std::make_shared<cxx::move_only_function<void(lux::ui::PaneId)>>(std::move(browse));
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(contracts::CodeLease::builtin(),
            [&catalog, &importer, receiver](const views::ViewFactoryInput& input)
                -> views::ViewFactoryResult<views::DetachedView> {
                auto pane = std::make_unique<ImportView>(input.dispatcher(), input.paneId(), catalog, importer);
                auto connection = object::LuxObject::connect(pane.get(), &ImportView::browseRequested,
                    [id = input.paneId(), receiver]() noexcept { (*receiver)(id); }
                );
                if (!connection)
                    return cxx::unexpected(workbench::detail::viewFailure(connection.error()));
                views::DetachedView view{contracts::CodeLease::builtin(), std::move(pane)};
                view.addConnection(std::move(*connection));
                return view;
            }
        );''')
for stem,name,label,intent in [('ResultsView','content.results','Content and Operations','VResultIntent'),('WorkspaceView','workspace','Workspace','VWorkspaceIntent')]:
 add('project/tools',stem,'project','lux.editor.'+name,label,f'    std::shared_ptr<views::ViewFactoryEntry> make{stem}Factory({stem}::Observe observe, {stem}::Request request)',f'''        struct Receivers final
        {{
            {stem}::Observe observe;
            {stem}::Request request;
        }};
        auto receivers = std::make_shared<Receivers>(std::move(observe), std::move(request));
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(contracts::CodeLease::builtin(),
            [receivers](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {{
                return views::DetachedView{{contracts::CodeLease::builtin(), std::make_unique<{stem}>(
                    input.dispatcher(), input.paneId(),
                    [receivers] {{ return receivers->observe(); }},
                    [receivers]({intent} intent) {{ return receivers->request(std::move(intent)); }}
                )}};
            }}
        );''')
add('project/tools','ProjectCreationView','project','lux.editor.project.creation','New project','''    std::shared_ptr<views::ViewFactoryEntry> makeProjectCreationViewFactory(
        cxx::move_only_function<ProjectCreationRequests()> requests
    )''','''        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(contracts::CodeLease::builtin(),
            [requests = std::move(requests)](const views::ViewFactoryInput& input) mutable
                -> views::ViewFactoryResult<views::DetachedView> {
                EditorResult<void> ready;
                auto pane = std::make_unique<ProjectCreationView>(input.dispatcher(), input.paneId(), requests(), ready);
                if (!ready)
                    return cxx::unexpected(workbench::detail::viewFailure(ready.error()));
                return views::DetachedView{contracts::CodeLease::builtin(), std::move(pane)};
            }
        );''')
p=s/'editor/application/src/EditorCommands.cpp';t=p.read_text();a=t.index('        draft.views.push_back(views::ViewFactoryEntry::create(');b=t.index('        const extensions::SessionActivities',a);t=t[:a]+'''        draft.views.push_back(project::makeProjectViewFactory(project_->catalogModel(),
            [this](const AssetReference& ref) {
                if (phase_ != EApplicationPhase::RUNNING || open_intents_.size() == 64)
                    log::error("application.open", "Asset open intent rejected: application closing or queue full");
                else
                    open_intents_.push_back(ref);
            }
        ));
        draft.views.push_back(tasks::makeTaskViewFactory(task_monitor_));
'''+t[b:];p.write_text(t)
p=s/'editor/application/src/EditorApplication.cpp';t=p.read_text();a=t.index('    views::ViewFactoryResult<views::DetachedView> EditorApplication::Impl::makeProjectView');b=t.index('    EditorResult<void> EditorApplication::Impl::admission',a);p.write_text(t[:a]+t[b:])
p=s/'editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp';t=p.read_text();t=t.replace('        [[nodiscard]] views::ViewFactoryResult<views::DetachedView> makeProjectView(lux::ui::PaneId);\n','');p.write_text(t)
p=s/'editor/application/src/EditorProjectTools.cpp';t=p.read_text();a=t.index('        draft.views.push_back(views::ViewFactoryEntry::create(');b=t.index('        draft.commands.push_back',a);t=t[:a]+'''        draft.views.push_back(project::makeImportViewFactory(project_->catalogModel(), *importer_,
            [this](lux::ui::PaneId pane) { import_browse_ = std::move(pane); }
        ));
'''+t[b:];a=t.index('        draft.views.push_back(views::ViewFactoryEntry::create(');b=t.index('        draft.commands.push_back',a);t=t[:a]+'''        draft.views.push_back(project::makeRecentProjectsViewFactory(*recent_projects_,
            [this](const std::filesystem::path& path) -> EditorResult<void> {
                const bool is_unavailable = phase_ != EApplicationPhase::RUNNING ||
                    project_launch_.has_value() || project_launch_intent_.has_value();
                if (is_unavailable)
                    return cxx::unexpected(EditorFailure{EEditorError::BUSY, "recent.open"});
                project_launch_intent_ = path;
                return {};
            }
        ));
'''+t[b:];p.write_text(t)
for file,stem,observe,intent,member,domain in [('EditorResults.cpp','ResultsView','observeResults','VResultIntent','result_intent_','result'),('EditorWorkspace.cpp','WorkspaceView','observeWorkspace','VWorkspaceIntent','workspace_intent_','workspace')]:
 p=s/'editor/application/src'/file;t=p.read_text();a=t.index('        draft.views.push_back(views::ViewFactoryEntry::create(');b=t.index('        draft.commands.push_back',a);t=t[:a]+f'''        draft.views.push_back(project::make{stem}Factory(
            [this] {{ return {observe}(); }},
            [this]({intent} intent) -> EditorResult<void> {{
                if ({member})
                    return cxx::unexpected(EditorFailure{{EEditorError::BUSY, "{domain}.intent.capacity"}});
                {member} = std::move(intent);
                return {{}};
            }}
        ));
'''+t[b:];p.write_text(t)
p=s/'editor/application/src/EditorProjectCreation.cpp';t=p.read_text();a=t.index('        draft.views.push_back(views::ViewFactoryEntry::create(');b=t.index('        draft.commands.push_back',a);t=t[:a]+'''        draft.views.push_back(project::makeProjectCreationViewFactory([this] {
            if (!project_creation_)
                project_creation_ = std::make_unique<ProjectCreation>(
                    engine_->execution(), messages_.dispatcherRef(), config_.installation, !config_.offscreen
                );
            return project_creation_->requests();
        }));
'''+t[b:];p.write_text(t)
for folder,target in [('tasks','tasks_ui'),('project','project_ui'),('project/tools','project_tools_ui')]:
 p=s/f'editor/workbench/{folder}/CMakeLists.txt';t=p.read_text().replace('PRIVATE imgui::core)','PRIVATE lux::engine::editor::view_host imgui::core)');t=t.replace('    "find_package(imgui REQUIRED COMPONENTS core)")','    "find_package(lux-engine-editor-desktop REQUIRED COMPONENTS view_host)"\n    "find_package(imgui REQUIRED COMPONENTS core)")');t=t.replace('lux_classify_target(',f'target_include_directories({target} PRIVATE ${{PROJECT_SOURCE_DIR}}/editor/workbench/sinclude)\nlux_classify_target(',1);p.write_text(t)
