from pathlib import Path
from collections import defaultdict
import json,re

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
work=Path(__file__).parent
plan=json.loads((work/'file-plan.json').read_text())
groups=defaultdict(lambda:defaultdict(list))
# Existing package identities, selected by their original declaration owner, not the new directory.
providers={
 'editor/contracts/':('editor_contracts','lux-engine-editor-contracts'),
 'editor/editing/history/':('edit_history','lux-engine-edit-history'),
 'editor/editing/sessions/':('edit_sessions','lux-engine-edit-sessions'),
 'editor/editing/scene/':('editor_editing_scene','lux-engine-editor-editing-scene'),
 'editor/editing/':('editor_editing','lux-engine-editor-editing'),
 'editor/persistence/':('editor_persistence','lux-engine-editor-persistence'),
 'editor/storage/':('editor_storage','lux-engine-editor-storage'),
 'editor/assets/':('editor_assets','lux-engine-editor-assets'),
 'editor/tasks/ui/':('tasks_ui','lux-engine-editor-tasks-ui'),
 'editor/tools/scene/execution/api/':('scene_execution_api','lux-engine-editor-scene-execution'),
 'editor/tools/scene/execution/':('scene_execution','lux-engine-editor-scene-execution'),
 'editor/tools/scene/projection/':('scene_projection','lux-engine-editor-scene-projection'),
 'editor/tools/scene/persistence/':('scene_persistence','lux-engine-editor-scene-persistence'),
 'editor/tools/material/persistence/':('material_persistence','lux-engine-editor-material-persistence'),
 'editor/tools/material/preview/':('material_preview','lux-engine-editor-material-preview'),
 'editor/tools/flowforge/persistence/':('flowforge_persistence','lux-engine-editor-flowforge-persistence'),
 'editor/tools/flowforge/compilation/':('flowforge_compilation','lux-engine-editor-flowforge-compilation'),
 'editor/desktop/':('desktop_shell','lux-engine-editor-desktop'),
 'editor/views/api/':('view_api','lux-engine-editor-view-api'),
}
overrides={
 'EditorError.hpp':('editor_contracts','lux-engine-editor-contracts'),
 'FilePublication.hpp':('editor_file_publication','lux-engine-editor-file-publication'),
 'FileArtifactStore.hpp':('editor_file_publication','lux-engine-editor-file-publication'),
 'SaveExecution.hpp':('editor_persistence_execution','lux-engine-editor-persistence'),
 'TaskMonitor.hpp':('editor_tasks','lux-engine-editor-tasks'),
 'ViewHost.hpp':('view_host','lux-engine-editor-desktop'),
}
owners={}
for item in plan:
 source,dest=item['source'],item['destination']
 if '/include/' not in source or not (repo/dest).exists():continue
 matched=next((p for p in sorted(providers,key=len,reverse=True) if source.startswith(p)),None)
 if not matched:continue
 target,package=overrides.get(Path(source).name,providers[matched])
 owners[dest]=target
 root,logical=dest.split('/include/',1)
 groups[root][package].append((dest,logical))
dest='editor/workbench/desktop/include/lux/engine/editor/views/ViewError.hpp'
owners[dest]='view_api'
groups['editor/workbench/desktop']['lux-engine-editor-view-api'].append((dest,dest.split('/include/',1)[1]))

for root,packages in groups.items():
 p=repo/root/'CMakeLists.txt'
 if not p.exists():continue
 text=p.read_text()
 # Only co-located providers need precision here. Single-provider directories retain their export.
 if len({owners[file] for group in packages.values() for file,_ in group}) < 2:continue
 text=text.replace('BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include','BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include')
 text+='\n# Exact SDK ownership: sibling providers share a build include root, never an install manifest.\n'
 for package,files in sorted(packages.items()):
  assert package in text,(root,package)
  destinations=defaultdict(list)
  for source,logical in files:destinations[Path(logical).parent.as_posix()].append(source)
  for logical,sources in sorted(destinations.items()):
   text+='install(FILES\n'
   for source in sorted(sources):text+='    ${CMAKE_CURRENT_SOURCE_DIR}/'+source.removeprefix(root+'/')+'\n'
   text+='    DESTINATION include/'+logical+' COMPONENT '+package.replace('-','_')+')\n'
 if root=='editor/editing':
  for logical,package in [('lux/engine/editor/editing/history_visibility.h','lux_engine_edit_history'),('lux/engine/editor/sessions/visibility.h','lux_engine_edit_sessions'),('lux/engine/editor/editing/visibility.h','lux_engine_editor_editing')]:
   text+='install(FILES ${LUX_GENERATE_HEADER_DIR}/'+logical+'\n    DESTINATION include/'+Path(logical).parent.as_posix()+' COMPONENT '+package+')\n'
 if root=='editor/activities/project':
  for name in ('storage','assets'):
   text+='install(FILES ${LUX_GENERATE_HEADER_DIR}/lux/engine/editor/'+name+'/visibility.h\n    DESTINATION include/lux/engine/editor/'+name+' COMPONENT lux_engine_editor_'+name+')\n'
 p.write_text(text,newline='\n')
(work/'header-owners.json').write_text(json.dumps(owners,indent=2)+'\n')
print('Exact merged-provider SDK header ownership:',len(owners))
