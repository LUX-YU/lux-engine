from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2'); w=Path('E:/SyncForder/CodeRepos/lux-engine/.internal/editor-redesign/EC2')
for kind in ['Result','Workspace']:
 file=s/f'editor/application/src/Editor{kind}s.cpp' if kind=='Result' else s/'editor/application/src/EditorWorkspace.cpp'
 t=file.read_text();a=t.index('        class '+('ResultsPane' if kind=='Result' else 'WorkspacePane')+' final');b=t.index('        draft.views.push_back',a);t=t[:a]+t[b:]
 plural='Results' if kind=='Result' else 'Workspace';lower='result' if kind=='Result' else 'workspace';old='ResultsPane' if kind=='Result' else 'WorkspacePane';intent='VResultIntent' if kind=='Result' else 'VWorkspaceIntent'
 t=t.replace(f'std::make_unique<{old}>(input.dispatcher(), input.paneId(), *this)',f'''std::make_unique<project::{plural}View>(input.dispatcher(), input.paneId(),
                        [this] {{ return observe{plural}(); }},
                        [this]({intent} intent) -> EditorResult<void> {{
                            if ({lower}_intent_)
                                return cxx::unexpected(EditorFailure{{EEditorError::BUSY, "{lower}.intent.capacity"}});
                            {lower}_intent_ = std::move(intent);
                            return {{}};
                        }}
                    )''')
 a=t.index(f'    void EditorApplication::Impl::install{kind}View');t=t[:a]+(w/f'pending/{plural}Observation.cpp').read_text()+'\n'+t[a:]
 for h in ['#include <lux/engine/ui/Element.hpp>\n','#include <imgui.h>\n','#include <imgui_stdlib.h>\n']:t=t.replace(h,'')
 file.write_text(t,newline='\n')
p=s/'editor/workbench/project/tools/CMakeLists.txt';t=p.read_text().replace('src/ProjectCreationView.cpp)','src/ProjectCreationView.cpp src/ResultsView.cpp src/WorkspaceView.cpp)');p.write_text(t,newline='\n')
p=s/'editor/tests/architecture/rules.json';j=json.loads(p.read_text());
for name in ['ResultsView','WorkspaceView']:
 for tail in [f'include/lux/engine/editor/project/{name}.hpp',f'src/{name}.cpp']:
  j['editor_layering']['files']['editor/workbench/project/tools/'+tail]=['project_tools_ui']
p.write_text(json.dumps(j,indent=2)+'\n',newline='\n')
