from pathlib import Path
import json,re
from collections import defaultdict

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
work=Path(__file__).parent
graph=json.loads(Path(r'E:/SyncForder/CodeRepos/build/RelWithDebInfo/p10q-structure-dev/editor-architecture/targets.json').read_text())
rules=json.loads((repo/'editor/tests/architecture/rules.json').read_text())
plan=json.loads((work/'file-plan.json').read_text())
headers=json.loads((work/'header-owners.json').read_text())
formal={
 'editor_contracts':('E0','VALUE',['CPU']), 'edit_history':('E0','PROVIDER',['CPU']),
 'edit_sessions':('E0','PROVIDER',['CPU']),
 **{x:('E1','PROVIDER',['CPU']) for x in ['scene_model','material_model','flowforge_model','editor_project','layout_model']},
 'editor_persistence':('E2','POLICY',['CPU']),
 **{x:('E2','PROVIDER',['CPU']) for x in ['scene_persistence','material_persistence','flowforge_persistence','editor_file_publication']},
 **{x:('E2','PROVIDER',['CPU','PROCESS']) for x in ['editor_persistence_execution','editor_storage','workspace_store','editor_tasks']},
 'editor_assets':('E2','PROVIDER',['CPU','PROCESS','TOOLCHAIN']),
 'scene_execution_api':('E2','VALUE',['CPU']),
 **{x:('E2','PROVIDER',['CPU','PROCESS','GPU']) for x in ['editor_editing_scene','scene_execution','scene_projection']},
 'material_preview':('E2','PROVIDER',['CPU','PROCESS','TOOLCHAIN','GPU']),
 'flowforge_compilation':('E2','PROVIDER',['CPU','PROCESS','TOOLCHAIN']),
 **{x:('E3','INTERACTION',['CPU']) for x in ['scene_interaction','material_interaction','flowforge_interaction']},
 **{x:('E3','UI',['CPU','GUI']) for x in ['view_api','view_host','editor_widgets','project_ui','tasks_ui']},
 **{x:('E3','UI',['CPU','GUI','GPU']) for x in ['desktop_shell','editor_viewport','scene_ui','material_ui','flow_ui']},
 'editor_launch':('E4','COMPOSITION',['CPU','PLATFORM']),
}
concrete=['scene_model','material_model','flowforge_model','scene_persistence','material_persistence','flowforge_persistence','editor_file_publication']
targets={}
for node in graph:
 name=node['name'];source=Path(node['SOURCE_DIR']).resolve()
 path=source.relative_to(repo).as_posix() if source.is_relative_to(repo) else None
 imported=node['IMPORTED'] in ('1','TRUE')
 if name in formal:
  layer,role,caps=formal[name]
 elif path and path.startswith('editor/') and not imported:
  if node['TYPE']=='EXECUTABLE' and name not in ('lux_editor','lux_launcher') or name.startswith('consumer_'):
   layer,role,caps='TEST','TEST',['CPU']
  elif node['TYPE']=='UTILITY' and path.startswith(('editor/activities/','editor/workbench/')):
   layer,role,caps=('E2' if '/activities/' in path else 'E3'),'GENERATOR',['CPU']
  else:layer,role,caps='RETAINED','PROVIDER',['CPU']
 else:
  layer='EXTERNAL' if imported else 'ENGINE'
  role='GENERATOR' if node['TYPE']=='UTILITY' else 'PROVIDER'
  caps=['CPU']
  value=('' if imported else (path or ''))+' '+name
  if re.search(r'engine/process/|process_execution',value):caps+=['PROCESS']
  if re.search(r'engine/toolchain/|flowforge_compiler|LLVM|MLIR|clang|spirv_cross|glslang',value,re.I):caps+=['TOOLCHAIN']
  if name != 'render_client' and re.search(r'modules/function/render/(?:runtime|renderer|vulkan|features)/|scene/render/|Vulkan|imgui_vulkan',value):caps+=['GPU']
  if re.search(r'modules/function/ui|imgui|node-editor',value):caps+=['GUI']
  if re.search(r'modules/platform/|glfw|nfd',value):caps+=['PLATFORM']
 targets[name]={'layer':layer,'role':role,'capabilities':caps}
 if imported:targets[name]['imported']=True
 elif path is not None:targets[name]['path']=path
 if layer=='RETAINED':
  targets[name]['deadline']='P11' if path in ('editor/metadata','editor/plugins') else 'P12'
  targets[name]['consumers']=sorted(x['name'] for x in graph if any(e.split(':',1)[1]==name for e in x.get('edges','').split(';') if e))

files=defaultdict(set)
for node in graph:
 name=node['name']
 for kind in ('SOURCES','INTERFACE_SOURCES'):
  for item in filter(None,node.get(kind,'').split(';')):
   if '$<' in item:continue
   p=Path(item);p=p if p.is_absolute() else Path(node['SOURCE_DIR'])/p;p=p.resolve()
   if p.is_relative_to(repo) and p.suffix in ('.hpp','.cpp','.h','.cc'):
    files[p.relative_to(repo).as_posix()].add(name)
for path,owner in headers.items():files[path].add(owner)
for name in formal:
 for path in rules.get(name,{}).get('files',[]):
  if '/include/' in path and not files[path]:files[path].add(name)
old_provider={
 'editor/tools/scene/execution/':'scene_execution', 'editor/tools/scene/projection/':'scene_projection',
 'editor/editing/history/':'edit_history','editor/editing/sessions/':'edit_sessions',
 'editor/storage/':'editor_storage','editor/persistence/':'editor_persistence',
 'editor/tools/material/preview/':'material_preview','editor/tools/flowforge/compilation/':'flowforge_compilation',
 'editor/desktop/':'desktop_shell','editor/views/api/':'view_api',
}
for item in plan:
 path=item['destination'];p=repo/path
 if p.suffix not in ('.h','.hpp','.cpp','.cc') or not p.exists() or files[path]:continue
 old=item['source']
 provider=next((v for k,v in sorted(old_provider.items(),key=lambda x:-len(x[0])) if old.startswith(k)),None)
 if not provider:
  scope=[(len(v.get('path','')),k) for k,v in targets.items() if v.get('path') and path.startswith(v['path']+'/') and v['role'] not in ('TEST','GENERATOR')]
  if scope:
   maximum=max(x[0] for x in scope);scope=[k for n,k in scope if n==maximum]
   if len(scope)==1:provider=scope[0]
 if provider:files[path].add(provider)
# Codegen support belongs to the UI provider that installs and instantiates it.
for p in (repo/'editor/workbench/scene/codegen').rglob('*.hpp'):
 files[p.relative_to(repo).as_posix()].add('scene_ui')
for p in (repo/'editor/ui/codegen').rglob('*.hpp'):
 files[p.relative_to(repo).as_posix()].add('editor_ui')

native=set()
for node in graph:
 for edge in filter(None,node.get('edges','').split(';')):
  name=edge.split(':',1)[1]
  if name not in targets:native.add(name)
print('Review explicit native leaves:',sorted(native))
rules['editor_layering']={
 'version':1,'targets':targets,'files':{k:sorted(v) for k,v in sorted(files.items()) if v},
 'native_libraries':sorted(x for x in native if '::' not in x), 'construction_exceptions':[],
 'shared_headers':{
  'editor/workbench/sinclude/lux/engine/editor/workbench/InteractionDelivery.hpp':{'layer':'E3','consumers':['material_ui','flow_ui']},
  **{('editor/activities/sinclude/lux/engine/editor/detail/'+h):{'layer':'E2','consumers':['editor_storage','editor_assets','editor_context','editor_app','lux_editor','editor_ui','editor_material','editor_flowforge','editor_scene','editor_project_tools','editor_settings','editor_launcher','editor_asset_save_test','consumer_gui']} for h in ['TaskResult.hpp','SignalDelivery.hpp']},
 },
 'concrete_save_providers':concrete,
 'author_state_providers':['scene_model','material_model','flowforge_model','edit_sessions','edit_history'],
 'generated_roots':[
  {'segment':'/editor/workbench/scene/inspector_gen/','suffix':'','owner':'scene_ui'},
  {'segment':'/editor/activities/project/meta_gen/','suffix':'','owner':'editor_storage'},
 ],
}
(repo/'editor/tests/architecture/rules.json').write_text(json.dumps(rules,ensure_ascii=False,indent=2)+'\n')
print('Targets',len(targets),'owned C++ files',len(files))
print('Unassigned',[(i['destination'],i['source']) for i in plan if i['layer'] in ['E0','E1','E2','E3','E4'] and Path(i['destination']).suffix in ('.hpp','.cpp','.h') and (repo/i['destination']).exists() and not files[i['destination']]])
