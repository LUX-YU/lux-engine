from pathlib import Path
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=s/'editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp';t=p.read_text()
a=t.index('        struct AcknowledgeMaintenance final');b=t.index('        struct RunPresentation final',a);result=t[a:b];t=t[:a]+t[b:]
a=t.index('        struct RefreshWorkspace final');b=t.index('        struct WorkspacePublication final',a);workspace=t[a:b];t=t[:a]+t[b:]
t=t.replace('#pragma once','#pragma once\n#include <lux/engine/editor/project/ResultsView.hpp>\n#include <lux/engine/editor/project/WorkspaceView.hpp>',1).replace('std::optional<VResultIntent>','std::optional<project::VResultIntent>').replace('std::optional<VWorkspaceIntent>','std::optional<project::VWorkspaceIntent>').replace('const VWorkspaceIntent&','const project::VWorkspaceIntent&')
t=t.replace('        void installResultView(', '        [[nodiscard]] EditorResult<project::ResultsSnapshot> observeResults();\n        [[nodiscard]] EditorResult<project::WorkspaceSnapshot> observeWorkspace();\n        void installResultView(')
p.write_text(t,newline='\n')
for name,actions,includes,values in [
('ResultsView',result,'#include <lux/engine/editor/persistence/SaveTypes.hpp>\n#include <lux/engine/editor/scene/RunTypes.hpp>', '''    struct ResultAction final { std::string label; VResultIntent intent; };
    struct ResultRow final
    {
        std::string key;
        std::vector<std::string> messages;
        std::vector<ResultAction> actions;
    };
    struct ResultSection final { std::string title; std::vector<ResultRow> rows; };
    struct ResultsSnapshot final { std::vector<ResultSection> sections; };
'''),
('WorkspaceView',workspace,'#include <lux/engine/editor/persistence/ArtifactStore.hpp>\n#include <lux/engine/editor/workspace/LayoutCatalog.hpp>', '''    struct WorkspacePublicationInfo final
    {
        std::string label;
        persistence::WriteTicket ticket;
        std::optional<persistence::VPublicationOutcome> result;
        std::optional<std::string> catalog_failure;
        bool unknown{};
    };
    struct WorkspaceSnapshot final
    {
        workspace::LayoutCatalog catalog;
        std::vector<std::string> diagnostics, recovery;
        std::vector<WorkspacePublicationInfo> publications;
    };
''')]:
 snapshot='ResultsSnapshot' if name=='ResultsView' else 'WorkspaceSnapshot';intent='VResultIntent' if name=='ResultsView' else 'VWorkspaceIntent'
 header='''#pragma once
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/cxx/core/move_only_function.hpp>
'''+includes+'''

namespace lux::editor::project
{
'''+''.join(line[4:]+'\n' if line.startswith('    ') else line+'\n' for line in actions.splitlines())+'\n'+values+f'''
    // A presentation snapshot is not a second result owner. Requests carry their original
    // domain identities; the receiver revalidates and executes them at its safe point.
    class {name} final : public lux::ui::Pane
    {{
    public:
        using Observe = cxx::move_only_function<EditorResult<{snapshot}>()>;
        using Request = cxx::move_only_function<EditorResult<void>({intent})>;
        {name}(object::ObjectDispatcherRef, lux::ui::PaneId, Observe, Request);
        ~{name}() noexcept override;
        {name}(const {name}&) = delete;
        {name}& operator=(const {name}&) = delete;
        {name}({name}&&) = delete;
        {name}& operator=({name}&&) = delete;
        [[nodiscard]] EditorResult<void> request({intent});
        [[nodiscard]] const {snapshot}& snapshot() const noexcept;
        [[nodiscard]] const std::optional<EditorFailure>& observationFailure() const noexcept;
    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    }};
}}
'''
 (s/f'editor/workbench/project/tools/include/lux/engine/editor/project/{name}.hpp').write_text(header,newline='\n')
for file in ['editor/application/src/EditorWorkspace.cpp','editor/application/src/EditorResults.cpp']:
 p=s/file;t=p.read_text().replace('namespace lux::editor::application\n{','namespace lux::editor::application\n{\n    using namespace lux::editor::project;',1);p.write_text(t,newline='\n')
p=s/'editor/tests/integration/application/application.cpp';t=p.read_text();import re
names=re.findall(r'struct (\w+) final',result+workspace)
for name in names:
 t=t.replace(f'ApplicationImpl::{name}',f'lux::editor::project::{name}').replace(f'std::remove_reference_t<decltype(impl)>::{name}',f'lux::editor::project::{name}')
p.write_text(t,newline='\n')
