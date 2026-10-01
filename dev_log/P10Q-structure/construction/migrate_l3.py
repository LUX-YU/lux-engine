from pathlib import Path
import json, re, subprocess

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
work=Path(__file__).resolve().parent
plan=json.loads((work/'file-plan.json').read_text())
def read(name):return (repo/name).read_text(encoding='utf-8-sig')
def write(name,text):
 p=repo/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text,encoding='utf-8',newline='\n')
def move(a,b):
 src,dst=repo/a,repo/b
 assert src.resolve().is_relative_to(repo) and dst.resolve().is_relative_to(repo)
 assert not dst.exists(),b
 dst.parent.mkdir(parents=True,exist_ok=True);src.rename(dst)

desktop=read('editor/views/api/CMakeLists.txt')+'\n'+read('editor/desktop/CMakeLists.txt')
desktop=desktop.replace('\n    ${PROJECT_SOURCE_DIR}/editor/workbench/desktop/include','')
scene=read('editor/tools/scene/ui/CMakeLists.txt')
start=scene.index('if(LUX_EDITOR_TEST_DESKTOP_GPU AND LUX_EDITOR_BUILD_TOOLCHAIN_TESTS)')
end=scene.index('include_component_cmake_scripts(meta)',start)
integration=scene[start:end].replace('test/views.cpp','views.cpp')
scene=scene[:start]+scene[end:]

for item in plan:
 if item['batch'] not in ('L3','L3+L5'):continue
 a,b=item['source'],item['destination']
 if not (repo/a).exists():raise RuntimeError(a)
 if a in ('editor/views/api/CMakeLists.txt','editor/desktop/CMakeLists.txt','editor/views/api/README.md','editor/desktop/README.md'):
  (repo/a).unlink();item['action']='MERGE';continue
 move(a,b)
write('editor/workbench/desktop/CMakeLists.txt',desktop)
write('editor/workbench/scene/CMakeLists.txt',scene)
write('editor/tests/integration/scene_views/CMakeLists.txt',integration)
p='editor/tests/integration/CMakeLists.txt';write(p,read(p)+'add_subdirectory(scene_views)\n')
mapping={
 'editor/views/api':'editor/workbench/desktop',
 'editor/desktop':'editor/workbench/desktop',
 'editor/views/viewport':'editor/workbench/viewport',
 'editor/widgets':'editor/workbench/widgets',
 'editor/tasks/ui':'editor/workbench/tasks',
 'editor/project/ui':'editor/workbench/project',
 'editor/tools/scene/ui/test':'editor/tests/integration/scene_views',
 'editor/tools/scene/interaction':'editor/workbench/scene/interaction',
 'editor/tools/material/interaction':'editor/workbench/material/interaction',
 'editor/tools/flowforge/interaction':'editor/workbench/flow/interaction',
 'editor/tools/scene/ui':'editor/workbench/scene',
 'editor/tools/material/ui':'editor/workbench/material',
 'editor/tools/flowforge/ui':'editor/workbench/flow',
 'lux/engine/editor/editing/InteractionDelivery.hpp':'lux/engine/editor/workbench/InteractionDelivery.hpp',
 'lux::editor::editing::detail':'lux::editor::workbench::detail',
 'editing::detail::EInputDeliveryStage':'workbench::detail::EInputDeliveryStage',
 'editing::detail::deliverInput':'workbench::detail::deliverInput',
}
files=set(subprocess.check_output(['git','ls-files','-z'],cwd=repo).decode().split('\0'))
files|={p.relative_to(repo).as_posix() for p in (repo/'editor').rglob('*') if p.is_file()}
for name in files:
 p=repo/name
 if not p.is_file() or name.startswith(('dev_log/','.internal/')):continue
 if p.suffix not in ('.txt','.cmake','.json','.py','.md','.hpp','.cpp'):continue
 s=read(name);new=s
 for a,b in sorted(mapping.items(),key=lambda v:-len(v[0])):new=re.sub(r'(?<!lux/engine/)'+re.escape(a),b,new)
 if name in ('editor/workbench/material/CMakeLists.txt','editor/workbench/flow/CMakeLists.txt'):
  new=new.replace('editor/editing/sinclude','editor/workbench/sinclude')
 if new!=s:write(name,new)
p='editor/CMakeLists.txt';s=read(p)
# Production is configured by responsibility; domain interaction providers precede their UI consumers.
for old in list(mapping):
 if old.startswith('editor/'):
  s=s.replace('add_subdirectory('+old.removeprefix('editor/')+')\n','')
# Root may have been rewritten by the path substitution above; remove their new forms as well.
for new in list(mapping.values()):
 if new.startswith('editor/'):
  s=s.replace('add_subdirectory('+new.removeprefix('editor/')+')\n','')
s=s.replace('add_subdirectory(activities)\n','add_subdirectory(activities)\nadd_subdirectory(workbench)\n')
write(p,s)
write('editor/workbench/CMakeLists.txt','''add_subdirectory(desktop)
add_subdirectory(viewport)
add_subdirectory(widgets)
add_subdirectory(scene/interaction)
add_subdirectory(material/interaction)
add_subdirectory(flow/interaction)
add_subdirectory(project)
add_subdirectory(tasks)
add_subdirectory(scene)
add_subdirectory(material)
add_subdirectory(flow)
''')
write('editor/workbench/desktop/README.md','''# Desktop

view_api provides detached ownership and the narrow host protocol. ViewInfo remains an Editing value;
ViewError owns close diagnostics here. ViewHost is the only top-level Pane owner; Root borrows its nodes.
desktop_shell composes the existing UI, platform and renderer frame boundaries without owning author sessions.
''')
write('editor/workbench/README.md','''# Workbench

Desktop, viewport and widgets are generic UI capabilities. Scene, Material, Flow, Project and Tasks
compose domain public services without duplicating author or asynchronous owners. Domain interaction
targets remain CPU-only, regardless of their physical workbench location.

InteractionDelivery is a private shared algorithm for the two graph tools, not a widget or installed API.
''')
(work/'file-plan.json').write_text(json.dumps(plan,ensure_ascii=False,indent=2))
print('L3 formal UI, CPU interactions and generators relocated; integration configured last.')
