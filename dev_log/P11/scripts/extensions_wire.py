from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-p11')
def edit(p,a,b):
 p=s/p;t=p.read_text();assert a in t,(p,a);p.write_text(t.replace(a,b),newline='\n')
for p in ['editor/activities/commands/include/lux/engine/editor/commands/CommandRegistry.hpp','editor/activities/sessions/include/lux/engine/editor/sessions/SessionFactory.hpp','editor/workbench/desktop/include/lux/engine/editor/views/ViewFactory.hpp']:
 t=(s/p).read_text();start=t.index('class '+('CommandEntry' if 'CommandRegistry' in p else 'SessionFactoryEntry' if 'SessionFactory' in p else 'ViewFactoryEntry'))
 at=t.index('    private:',start);t=t[:at]+'        [[nodiscard]] bool usesCode(const contracts::CodeLease& code) const noexcept { return code_.sameOwner(code); }\n'+t[at:];(s/p).write_text(t,newline='\n')
rpath=s/'editor/tests/architecture/rules.json';r=json.loads(rpath.read_text())
r['scene_ui']['closure']['meta']={'path':'modules/core/meta'};r['scene_ui']['closure']['lux::cxx::reflection_runtime']={'path':'.'}
l=r['editor_layering'];l['targets']['editor_extensions']={'layer':'E4','role':'COMPOSITION','capabilities':['CPU','PROCESS','TOOLCHAIN','GPU'],'path':'editor/application/extensions'}
for p in (s/'editor/application/extensions').rglob('*'):
 if p.suffix in ['.hpp','.cpp']:l['files'][p.relative_to(s).as_posix()]=['editor_extensions']
l['generated_roots'].append({'segment':'/gen/include/lux/engine/editor/extensions/EditorExtensionAbi.hpp','suffix':'/gen/include/lux/engine/editor/extensions/EditorExtensionAbi.hpp','owner':'editor_extensions','public':True})
rpath.write_text(json.dumps(r,indent=2)+'\n')
edit('editor/application/CMakeLists.txt','add_subdirectory(launch)','add_subdirectory(launch)\nadd_subdirectory(extensions)')
