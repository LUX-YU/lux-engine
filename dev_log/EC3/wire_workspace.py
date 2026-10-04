from pathlib import Path
import json
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p = s/'editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp'
t = p.read_text()
t = t.replace('#pragma once', '#pragma once\n#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>\n#include <lux/engine/editor/desktop/WorkspaceActions.hpp>', 1)
a=t.index('        struct WorkspacePublication final')
b=t.index('        struct RecoveryItem final', a)
t=t[:a]+t[b:]
t=t.replace('        workspace::LayoutCatalog layout_catalog_;','        workspace::WorkspaceChanges workspace_changes_;\n        std::unique_ptr<desktop::WorkspaceActions> workspace_actions_;')
for line in ['        std::vector<WorkspacePublication> workspace_publications_;',
 '        std::optional<workspace::LegacyMigration> migration_;',
 '        std::optional<persistence::WriteTicket> migration_ticket_;',
 '        std::optional<EditorFailure> migration_failure_;', '        bool migration_complete_{};',
 '        [[nodiscard]] EditorResult<void> settleMigration();']:
    assert line in t
    t=t.replace(line+'\n','')
p.write_text(t)
p=s/'editor/application/src/EditorApplication.cpp'
t=p.read_text().replace('workspace_(config_.project_file.parent_path(), writes_, files_)', 'workspace_(config_.project_file.parent_path(), writes_, files_),\n          workspace_changes_(workspace_, writes_, files_)')
t=t.replace('        workspace_publications_.reserve(16);\n','')
t=t.replace('        desktop_ = std::move(*desktop);', '''        desktop_ = std::move(*desktop);
        workspace_actions_ = std::make_unique<desktop::WorkspaceActions>(
            desktop_->views(), workspace_, workspace_changes_, messages_.dispatcherRef()
        );''')
p.write_text(t)
p=s/'editor/application/src/EditorWorkspace.cpp'
t=p.read_text()
a=t.index('            auto create_input =')
b=t.index('\n            return {};',t.index('            auto committed =',a))
t=t[:a]+'''            result = workspace_actions_->apply(std::move(layout), snapshot.views());'''+t[b:]
a=t.index('        using namespace workspace;',t.index('::executeWorkspaceIntent('))
b=t.index('\n    EditorResult<void> EditorApplication::Impl::settleWorkspace()',a)
t=t[:a]+'''        return std::visit([&](const auto& intent) -> EditorResult<void> {
            using Intent = std::decay_t<decltype(intent)>;
            if constexpr (std::same_as<Intent, RefreshWorkspace>)
                return workspace_changes_.refresh();
            else if constexpr (std::same_as<Intent, AcknowledgeWorkspace>)
                return workspace_changes_.acknowledge(intent.ticket);
            else if constexpr (std::same_as<Intent, ReconcileWorkspace>)
                return workspace_changes_.reconcile(intent.ticket);
            else
            {
                if (phase_ != EApplicationPhase::RUNNING)
                    return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "workspace.admission"});
                if constexpr (std::same_as<Intent, CaptureRecovery>)
                    return captureRecovery();
                else if constexpr (std::same_as<Intent, RestoreRecovery>)
                    return restoreRecovery();
                else if constexpr (std::same_as<Intent, MigrateWorkspace>)
                    return workspace_changes_.migrate();
                else if constexpr (std::same_as<Intent, SaveLayout>)
                    return workspace_actions_->save(intent.label);
                else if constexpr (std::same_as<Intent, RenameLayout>)
                    return workspace_changes_.rename(intent.layout, intent.label);
                else if constexpr (std::same_as<Intent, RemoveLayout>)
                    return workspace_changes_.remove(intent.layout);
                else if constexpr (std::same_as<Intent, ApplyLayout>)
                {
                    EditorResult<void> result;
                    auto apply = [&](const extensions::ContributionSnapshot& snapshot)
                        -> extensions::ContributionResult<void> {
                        result = workspace_actions_->apply(intent.layout, snapshot.views());
                        return {};
                    };
                    auto guarded = contributions_.withSnapshot(apply);
                    return guarded ? std::move(result) : applicationFailure("layout.catalog", guarded.error());
                }
                else
                    static_assert(sizeof(Intent) == 0, "Every workspace intent needs an explicit receiver");
            }
        }, request);
    }
'''+t[b:]
a=t.index('        for (auto& report : workspace_publications_)')
b=t.index('        return settleRecovery();',a)
t=t[:a]+'''        auto updated = workspace_changes_.update(phase_ == EApplicationPhase::RUNNING);
        if (!updated)
            return updated;
'''+t[b:]
t=t.replace('snapshot.catalog = layout_catalog_;','snapshot.catalog = workspace_changes_.catalog();')
t=t.replace('if (migration_)','if (const auto* migration = workspace_changes_.migration())')
t=t.replace('migration_->','migration->').replace('if (migration_complete_)','if (workspace_changes_.migrationComplete())')
t=t.replace('if (migration_failure_)','if (const auto* error = workspace_changes_.migrationFailure())').replace('migration_failure_->','error->')
t=t.replace('workspace_publications_', 'workspace_changes_.publications()')
t=t.replace('#include <random>\n','')
p.write_text(t)
p=s/'editor/application/src/EditorRecovery.cpp'
t=p.read_text()
a=t.index('        auto written = workspace_.writeRecovery')
b=t.index('\n    EditorResult<void> EditorApplication::Impl::restoreRecovery()',a)
t=t[:a]+'''        return workspace_changes_.recordRecovery(value, version);
    }
'''+t[b:]
a=t.index('    EditorResult<void> EditorApplication::Impl::settleMigration()')
t=t[:a]+'}\n'
p.write_text(t)
p=s/'editor/application/src/EditorLifecycle.cpp'
t=p.read_text().replace('workspace_publications_','workspace_changes_.publications()')
p.write_text(t)
p=s/'editor/tests/integration/application/application.cpp'
t=p.read_text().replace('impl.layout_catalog_', 'impl.workspace_changes_.catalog()')
t=t.replace('''    auto recovery_write = impl.workspace_.writeRecovery(recovery_manifest, "missing");
    assert(recovery_write);
    impl.workspace_publications_.push_back({"Recovery fixture", *recovery_write});''','''    assert(impl.workspace_changes_.recordRecovery(recovery_manifest, "missing"));''')
t=t.replace('''            auto ticket = impl.workspace_.writeRecovery(composite, stored->target.expected_version);
            assert(ticket);
            impl.workspace_publications_.push_back({"Composite recovery", *ticket});''','''            assert(impl.workspace_changes_.recordRecovery(composite, stored->target.expected_version));''')
t=t.replace('impl.workspace_publications_', 'impl.workspace_changes_.publications()')
p.write_text(t)
p=s/'editor/activities/workspace/CMakeLists.txt'
t=p.read_text().replace('src/WorkspaceStore.cpp ', 'src/WorkspaceStore.cpp src/WorkspaceChanges.cpp ')
p.write_text(t)
p=s/'editor/workbench/desktop/CMakeLists.txt'
t=p.read_text().replace('SOURCE_FILES src/CommandMenu.cpp ', 'SOURCE_FILES src/WorkspaceActions.cpp src/CommandMenu.cpp ')
t=t.replace('target_link_libraries(desktop_shell PUBLIC ', 'target_link_libraries(desktop_shell PUBLIC lux::engine::editor::workspace_store ')
t=t.replace('component_add_transitive_commands(desktop_shell\n', 'component_add_transitive_commands(desktop_shell\n    "find_package(lux-engine-editor-workspace-store REQUIRED COMPONENTS workspace_store)"\n')
t=t.replace('install(FILES\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/CommandMenu.hpp', 'install(FILES\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WorkspaceActions.hpp\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/CommandMenu.hpp')
p.write_text(t)
p=s/'editor/tests/architecture/rules.json'
d=json.loads(p.read_text())
def find(mapping):
    if isinstance(mapping,dict):
        if 'editor/activities/project/src/ProjectContentSaving.cpp' in mapping:return mapping
        for v in mapping.values():
            r=find(v)
            if r is not None:return r
providers=find(d)
assert providers is not None
for f,provider in [('editor/activities/workspace/include/lux/engine/editor/workspace/WorkspaceChanges.hpp','workspace_store'),('editor/activities/workspace/src/WorkspaceChanges.cpp','workspace_store'),('editor/workbench/desktop/include/lux/engine/editor/desktop/WorkspaceActions.hpp','desktop_shell'),('editor/workbench/desktop/src/WorkspaceActions.cpp','desktop_shell')]:
    providers[f]=[provider]
p.write_text(json.dumps(d,indent=2)+'\n')
