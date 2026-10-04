from pathlib import Path
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
# Remove obsolete archive from its definition.
p=s/'editor/application/extensions/CMakeLists.txt';a=p.read_text();start=a.index('add_component(COMPONENT_NAME editor_builtin_contributions');end=a.index('lux_classify_target(TARGET editor_extensions',start);a=a[:start]+a[end:];a=a.replace('COMPONENTS editor_extensions editor_builtin_contributions)','COMPONENTS editor_extensions)');p.write_text(a)
providers='lux::engine::editor::editor_extensions lux::engine::editor::scene_ui lux::engine::editor::material_ui lux::engine::editor::flow_ui lux::engine::editor::scene_persistence lux::engine::editor::material_persistence lux::engine::editor::flowforge_persistence'
finds='''"find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions)"
    "find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui)"
    "find_package(lux-engine-editor-material-ui REQUIRED COMPONENTS material_ui)"
    "find_package(lux-engine-editor-flow-ui REQUIRED COMPONENTS flow_ui)"
    "find_package(lux-engine-editor-scene-persistence REQUIRED COMPONENTS scene_persistence)"
    "find_package(lux-engine-editor-material-persistence REQUIRED COMPONENTS material_persistence)"
    "find_package(lux-engine-editor-flowforge-persistence REQUIRED COMPONENTS flowforge_persistence)"'''
p=s/'editor/application/CMakeLists.txt';a=p.read_text().replace('lux::engine::editor::editor_builtin_contributions',providers).replace('"find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_builtin_contributions)"',finds);p.write_text(a)
for path in ['editor/tests/integration/scene_views/CMakeLists.txt','editor/tests/integration/application/CMakeLists.txt','editor/tests/integration/session_factories/CMakeLists.txt']:
 p=s/path;a=p.read_text().replace('editor_builtin_contributions','editor_extensions');p.write_text(a)
# Application test formerly obtained concrete headers via obsolete aggregate; name the real providers.
p=s/'editor/tests/integration/application/CMakeLists.txt';a=p.read_text().replace('PRIVATE editor_bootstrap','PRIVATE scene_ui material_ui flow_ui scene_persistence material_persistence flowforge_persistence editor_bootstrap');p.write_text(a)
# SDK consumers explicitly request/link what their maintained tests actually use.
for path in ['cmake/installed-consumers/desktop-views/CMakeLists.txt','cmake/installed-consumers/editor-p11/models/CMakeLists.txt']:
 p=s/path;a=p.read_text().replace('find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_builtin_contributions)',finds.replace('"','').replace('\n    ','\n'))
 if 'desktop-views' in path:
  a=a.replace('lux::engine::editor::editor_builtin_contributions','lux::engine::editor::editor_extensions')
 else:
  a=a.replace('lux::engine::editor::editor_builtin_contributions','lux::engine::editor::editor_extensions')
  a=a.replace('target_link_libraries(p11_models PRIVATE','target_link_libraries(p11_models PRIVATE lux::engine::editor::scene_persistence lux::engine::editor::material_persistence lux::engine::editor::flowforge_persistence')
 p.write_text(a)
# Tight actual rule deltas; deleting an obsolete node does not authorize a reverse-layer edge.
import json
p=s/'editor/tests/architecture/rules.json';d=json.loads(p.read_text());old='editor_builtin_contributions'
def clean(x):
 if isinstance(x,dict):return {k:clean(v) for k,v in x.items() if k!=old and 'BuiltinContributions.' not in k}
 if isinstance(x,list):return [clean(v) for v in x if v!=old and not (isinstance(v,str) and 'BuiltinContributions.' in v)]
 return x
d=clean(d)
for target,dependencies in [('scene_ui',['session_factories','scene_persistence']),('material_ui',['view_host']),('flow_ui',['view_host'])]:
 d[target]['direct']=list(dict.fromkeys(d[target]['direct']+dependencies))
 added=[]
 for dep in dependencies:
  added += [dep]+d.get(dep,{}).get('closure',[])
 # Session factories already have verified old source-provider closures.
 if 'session_factories' in dependencies:added += ['editor_commands','editor_persistence','edit_sessions','asset']
 d[target]['closure']=list(dict.fromkeys(d[target]['closure']+added))
 for parent in d.values():
  if isinstance(parent,dict) and target in parent.get('closure',[]):
   parent['closure']=list(dict.fromkeys(parent['closure']+added))
 for h in ['lux/engine/editor/views/ViewFactory.hpp','lux/engine/editor/workbench/ViewFactorySupport.hpp']:
  if h not in d[target]['headers']:d[target]['headers'].append(h)
for h in ['lux/engine/editor/sessions/SessionCommands.hpp','lux/engine/editor/scene/SceneSessionFactory.hpp']:
 if h not in d['scene_ui']['headers']:d['scene_ui']['headers'].append(h)
d['editor_layering']['shared_headers']['editor/workbench/sinclude/lux/engine/editor/workbench/ViewFactorySupport.hpp']={'layer':'E3','consumers':['scene_ui','material_ui','flow_ui']}
p.write_text(json.dumps(d,indent=2)+'\n')
# Descriptions reflect the actual remaining boundary, not a forwarding archive.
p=s/'editor/application/extensions/README.md';a=p.read_text().replace('`editor_builtin_contributions` holds the actual built-in factory composition. Control factory\ndescriptions live in','Each Scene, Material and Flow provider declares and binds its own factories and commands.\nApplication composes these exact providers. Control factory descriptions live in');start=a.index('`BuiltinContributions` installs');end=a.index('\n\n',start);a=a[:start]+'''`session_factories` provides Save/Undo/Redo; each concrete author activity provides its source factory
and creation command. Each workbench tool constructs its own detached view and signal connections.
There is no central built-in factory archive. External extensions use the same immutable descriptors
and Entry binding API, retaining their original code lease and activation dependencies.'''+a[end:];p.write_text(a)
