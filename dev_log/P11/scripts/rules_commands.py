from pathlib import Path
import json
p=Path('E:/SyncForder/CodeRepos/lux-engine-p11/editor/tests/architecture/rules.json')
r=json.loads(p.read_text())
r['editor_layering']['targets']['editor_session_installation_test']['path']='editor/tests/integration/session_factories'
for f in ['editor/workbench/desktop/src/CommandMenu.cpp','editor/workbench/desktop/include/lux/engine/editor/desktop/CommandMenu.hpp']:
 r['editor_layering']['files'][f]=['desktop_shell']
 if f not in r['desktop_shell']['files']:r['desktop_shell']['files'].append(f)
for key in ['desktop_shell','view_host']:
 for h in ['lux/engine/editor/desktop/CommandMenu.hpp','lux/engine/editor/commands/CommandRegistry.hpp','lux/engine/ui/Menu.hpp']:
  if h not in r[key]['headers']:r[key]['headers'].append(h)
for key in ['desktop_shell','editor_scene_views_test']:
 for name,path in [('editor_commands','editor/activities/commands'),('edit_sessions','editor/editing'),('edit_history','editor/editing')]:r[key]['closure'][name]={'path':path}
if 'editor_commands' not in r['desktop_shell']['direct']:r['desktop_shell']['direct'].append('editor_commands')
p.write_text(json.dumps(r,indent=2)+'\n')
