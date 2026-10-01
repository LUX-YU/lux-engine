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

groups={
 'persistence':['editor/persistence','editor/storage'],
 'project':['editor/storage','editor/assets'],
 'scene':['editor/tools/scene/persistence','editor/editing/scene','editor/tools/scene/execution/api',
          'editor/tools/scene/execution','editor/tools/scene/projection'],
 'material':['editor/tools/material/persistence','editor/tools/material/preview'],
 'flow':['editor/tools/flowforge/persistence','editor/tools/flowforge/compilation'],
 'workspace':['editor/workspace/storage'],
}
originals={path:read(path+'/CMakeLists.txt') for paths in groups.values() for path in paths}
storage=originals['editor/storage'];split=storage.index('generate_visibility_header(')
file_backend,project=storage[:split],storage[split:]
start=project.index('if(LUX_EDITOR_BUILD_NATIVE_TESTS)\n    add_executable(editor_publication_test')
file_backend+='\n'+project[start:];project=project[:start]
project=project.replace('lux::engine::editor::editor_editing','lux::engine::editor::editor_contracts')
project=project.replace('lux-engine-editor-editing REQUIRED COMPONENTS editor_editing',
    'lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts')
assets=originals['editor/assets'];split=assets.index('if(LUX_EDITOR_BUILD_NATIVE_TESTS)')
asset_core,asset_tests=assets[:split],assets[split:]
asset_core=asset_core.replace('${CMAKE_CURRENT_SOURCE_DIR}/sinclude ${PROJECT_SOURCE_DIR}/editor/storage/sinclude',
    '${CMAKE_CURRENT_SOURCE_DIR}/sinclude')
asset_tests=asset_tests.replace('PRIVATE editor_assets)', 'PRIVATE editor_assets editor_editing)')
production={
 'persistence': originals['editor/persistence']+'\n'+file_backend,
 'project':project+'\n'+asset_core,
 'workspace':originals['editor/workspace/storage'],
}
integration={}
for domain in ['scene','material','flow']:
 chunks=[]
 for path in groups[domain]:
  text=originals[path].replace('add_subdirectory(api)\n','')
  if path.endswith(('projection','preview')):
   start=text.find('if(LUX_EDITOR_BUILD_NATIVE_TESTS')
   if start!=-1:
    integration['projection' if path.endswith('projection') else 'material_activity']=text[start:].replace('test/','')
    text=text[:start]
  chunks.append(text)
 production[domain]='\n'.join(chunks)

for item in plan:
 if item['batch']!='L2':continue
 a,b=item['source'],item['destination']
 if item['action'].startswith('TEMPORARY'):continue
 if a=='editor/tasks/ui/test/monitor.cpp':
  b='editor/tests/integration/tasks/monitor.cpp';item['destination']=b;item['action']='MOVE_EDIT'
 if a=='editor/storage/test/publication.cpp':b='editor/activities/persistence/test/publication.cpp';item['destination']=b
 if a.endswith(('CMakeLists.txt','README.md')):
  if a=='editor/assets/CMakeLists.txt':continue
  if a=='editor/assets/README.md':continue
  (repo/a).unlink();item['action']='MERGE'
  if '/execution/api/' in a:item['destination']='editor/activities/scene/CMakeLists.txt'
  continue
 if a!=b:move(a,b)
for domain,text in production.items():write('editor/activities/'+domain+'/CMakeLists.txt',text)
write('editor/assets/CMakeLists.txt','# Tests of the remaining P12 product save adapter; not a new activity provider.\n'+asset_tests)
for name,text in integration.items():write('editor/tests/integration/'+name+'/CMakeLists.txt',text)
write('editor/tests/integration/tasks/CMakeLists.txt','''if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_task_monitor_test monitor.cpp)
    target_link_libraries(editor_task_monitor_test PRIVATE tasks_ui view_host)
    lux_editor_test_options(editor_task_monitor_test)
    add_test(NAME editor.tasks.monitor COMMAND editor_task_monitor_test)
    set_tests_properties(editor.tasks.monitor PROPERTIES LABELS "editor;native;P10Q" TIMEOUT 60)
endif()
''')
write('editor/tests/integration/CMakeLists.txt',''.join('add_subdirectory('+x+')\n' for x in ['projection','material_activity','tasks']))
write('editor/activities/tasks/CMakeLists.txt','''add_component(COMPONENT_NAME editor_tasks NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/TaskMonitor.cpp)
component_include_directories(editor_tasks BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_tasks PUBLIC lux::engine::core::object lux::engine::process::process_execution)
component_add_transitive_commands(editor_tasks
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)")
lux_classify_target(TARGET editor_tasks LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-tasks VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_tasks)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_task_monitor_cpu_test test/monitor.cpp)
    target_link_libraries(editor_task_monitor_cpu_test PRIVATE editor_tasks)
    lux_editor_test_options(editor_task_monitor_cpu_test)
    add_test(NAME editor.tasks.monitor_cpu COMMAND editor_task_monitor_cpu_test)
    set_tests_properties(editor.tasks.monitor_cpu PROPERTIES LABELS "editor;native;editor_layering" TIMEOUT 60)
endif()
''')
p='editor/tasks/ui/CMakeLists.txt';s=read(p).replace(' src/TaskMonitor.cpp','')
s=s[:s.index('if(LUX_EDITOR_BUILD_NATIVE_TESTS)')]
s=s.replace('lux::engine::process::process_execution','lux::engine::editor::editor_tasks')
s=s.replace('lux-engine-process-execution REQUIRED COMPONENTS process_execution','lux-engine-editor-tasks REQUIRED COMPONENTS editor_tasks')
write(p,s)
write('editor/activities/CMakeLists.txt',''.join('add_subdirectory('+x+')\n' for x in ['persistence','scene','material','flow','project','workspace','tasks']))
p='editor/CMakeLists.txt';s=read(p)
for paths in groups.values():
 for path in paths:s=s.replace('add_subdirectory('+path.removeprefix('editor/')+')\n','')
s=s.replace('add_subdirectory(authoring)\n','add_subdirectory(authoring)\nadd_subdirectory(activities)\n')
s=s.replace('add_subdirectory(app)\n','add_subdirectory(app)\nadd_subdirectory(assets) # P12 product adapter tests only.\nadd_subdirectory(tests/integration)\n')
write(p,s)
mapping={path:'editor/activities/'+domain for domain,paths in groups.items() for path in paths}
mapping.update({'editor/tools/scene/execution/api':'editor/activities/scene',
 'editor/tools/scene/projection/test':'editor/tests/integration/projection',
 'editor/tools/material/preview/test':'editor/tests/integration/material_activity',
 'editor/tasks/ui/test':'editor/tests/integration/tasks',
 'editor/tasks/ui/include/lux/engine/editor/tasks/TaskMonitor.hpp':'editor/activities/tasks/include/lux/engine/editor/tasks/TaskMonitor.hpp',
 'editor/tasks/ui/src/TaskMonitor.cpp':'editor/activities/tasks/src/TaskMonitor.cpp',
 'editor/storage':'editor/activities/project',
 'editor/assets/src':'editor/activities/project/src',
 'editor/assets/include':'editor/activities/project/include'})
# File publication comes from the persistence theme, not the project file backend.
for file in ['src/FilePublication.cpp','src/FileArtifactStore.cpp','include/lux/engine/editor/storage/FilePublication.hpp',
             'include/lux/engine/editor/storage/FileArtifactStore.hpp','test/publication.cpp']:
 mapping['editor/storage/'+file]='editor/activities/persistence/'+file
mapping.pop('editor/assets',None)
files=set(subprocess.check_output(['git','ls-files','-z'],cwd=repo).decode().split('\0'))
files|={p.relative_to(repo).as_posix() for p in (repo/'editor/activities').rglob('*') if p.is_file()}
for name in files:
 p=repo/name
 if not p.is_file() or name.startswith(('dev_log/','.internal/')):continue
 if p.suffix not in ('.txt','.cmake','.json','.py','.md','.hpp','.cpp'):continue
 s=read(name);new=s
 for a,b in sorted(mapping.items(),key=lambda v:-len(v[0])):new=re.sub(r'(?<!lux/engine/)'+re.escape(a),b,new)
 # Keep InteractionDelivery in E0 only until L3; other private helpers use activities' real directory.
 if name.endswith('CMakeLists.txt') and name not in ('editor/tools/material/ui/CMakeLists.txt','editor/tools/flowforge/ui/CMakeLists.txt'):
  new=new.replace('editor/editing/sinclude','editor/activities/sinclude')
 if new!=s:write(name,new)
# File publication's declared source directory follows its own target, even when original storage was shared.
p='editor/tests/architecture/rules.json';rules=json.loads(read(p))
for t in rules['targets']:
 if t['name']=='editor_file_publication':t['path']='editor/activities/persistence'
for value in rules.values():
 if isinstance(value,dict) and isinstance(value.get('closure'),dict):
  if 'editor_file_publication' in value['closure']:value['closure']['editor_file_publication']['path']='editor/activities/persistence'
write(p,json.dumps(rules,ensure_ascii=False,indent=2)+'\n')
(work/'file-plan.json').write_text(json.dumps(plan,ensure_ascii=False,indent=2))
for domain in production:
 write('editor/activities/'+domain+'/README.md',f'# {domain.title()} activities\n\nExisting domain activities, relocated with their separate real targets and ownership.\nAuthor state remains in authoring; workbench and application are consumers, never dependencies.\n')
print('L2 ownership-preserving migration prepared; task CPU fixture still required.')
