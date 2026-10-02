from pathlib import Path
import re, json, subprocess
r=Path(r'E:/SyncForder/CodeRepos/lux-engine-p12')
w=Path(r'E:/SyncForder/CodeRepos/lux-engine/.internal/editor-redesign/P12')
# Record original assertion locations before removing protocol owners. Revision, bytes and expressions remain auditable.
roots=['app','assets','context','launcher','metadata','plugins','tools','transition','ui']
files=subprocess.check_output(['git','ls-files','-z',*[f'editor/{x}' for x in roots]],cwd=r).decode().split('\0')
assertions=[]
for f in filter(None,files):
 if '/test/' not in f: continue
 s=(r/f).read_text(errors='replace')
 entries=[]; at=0
 for m in re.finditer(r'\bassert\s*\(',s):
  start=m.end(); depth=1; end=start
  while depth and end<len(s):
   depth+=(s[end]=='(')-(s[end]==')'); end+=1
  entries.append({'line':s.count('\n',0,m.start())+1,'expression':s[start:end-1]})
 assertions.append({'path':f,'implementation_sha':'ff09f087b59ad25cfd6df2709d54d77f8f92e141','assertions':entries,'migration':'PENDING_SEMANTIC_MAPPING'})
(w/'g-original-assertions.json').write_text(json.dumps(assertions,indent=2,ensure_ascii=False)+'\n')
# Move unchanged rendering qualification and codegen tests to their real production providers.
moves={'editor/ui/test/unified_ui.cpp':'editor/tests/integration/scene_views/unified_ui.cpp','editor/ui/test/inspector_codegen.py':'editor/workbench/scene/test/inspector_codegen.py','editor/tools/scene/test/fixture.cpp':'editor/tests/integration/scene_views/fixture.cpp','editor/tools/scene/test/scene_source_checks.hpp':'editor/tests/integration/scene_views/scene_source_checks.hpp','editor/tools/scene/test/run_system.hpp':'editor/tests/integration/scene_views/run_system.hpp'}
for a,b in moves.items():
 (r/b).parent.mkdir(parents=True,exist_ok=True);subprocess.run(['git','mv',a,b],cwd=r,check=True)
p=r/moves['editor/ui/test/unified_ui.cpp'];s=p.read_text().replace('../../../cmake/','../../../../cmake/');p.write_text(s)
p=r/moves['editor/ui/test/inspector_codegen.py'];s=p.read_text().replace('Path(__file__).parents[2] / "workbench/scene/codegen/inspector_codegen.py"','Path(__file__).parents[1] / "codegen/inspector_codegen.py"').replace('InspectorInteraction','InspectorFields');p.write_text(s)
# The existing project publication owner supersedes plugin-only private write operation.
p=r/'editor/activities/project/include/lux/engine/editor/storage/ProjectStorage.hpp';s=p.read_text();s=re.sub(r'namespace lux::editor::detail\n\{\n    class ProjectWrite;\n\}\n\n','',s);s=re.sub(r'        \[\[nodiscard\]\] EditorResult<void> savePlugins[\s\S]+?        void abandonPluginSave\(\) noexcept;\n','',s);s=s.replace('        std::unique_ptr<detail::ProjectWrite> plugin_save_;\n','');p.write_text(s)
p=r/'editor/activities/project/src/ProjectStorage.cpp';s=p.read_text().replace('#include <lux/engine/editor/detail/ProjectWrite.hpp>\n','').replace('        plugin_save_.reset(); // Adopt accepted publication before unmounting the project.\n','');a=s.index('    EditorResult<void> ProjectStorage::savePlugins(');b=s.index('    void ProjectStorage::requestClose()',a);s=s[:a]+s[b:];a=s.index('        if (plugin_save_ &&');b=s.index('        if (publishing_)',a);s=s[:a]+s[b:];p.write_text(s)
p=r/'editor/activities/project/src/ProjectOpenData.cpp';s=p.read_text().replace('#include <lux/engine/editor/detail/ProjectWrite.hpp>\n','');p.write_text(s)
# Remove only expired protocols; formal runtime SceneEditing remains an E2 target.
p=r/'editor/editing/CMakeLists.txt';s=p.read_text();a=s.index('# Existing product adaptation only;');b=s.index('# Exact SDK ownership:',a);s=s[:a]+s[b:];s=re.sub(r'install\(FILES\n    \$\{CMAKE_CURRENT_SOURCE_DIR\}/include/lux/engine/editor/AssetEditing.hpp[\s\S]+?COMPONENT lux_engine_editor_editing\)\n','',s);s=re.sub(r'install\(FILES\n    \$\{CMAKE_CURRENT_SOURCE_DIR\}/include/lux/engine/editor/editing/EditHistoryTarget.hpp[\s\S]+?COMPONENT lux_engine_editor_editing\)\n','',s);s=re.sub(r'install\(FILES \$\{LUX_GENERATE_HEADER_DIR\}/lux/engine/editor/editing/visibility.h\n    [^\n]+\)\n','',s);p.write_text(s)
expired=['editor/editing/src/EditHistoryTarget.cpp','editor/editing/include/lux/engine/editor/AssetEditing.hpp','editor/editing/include/lux/engine/editor/AssetOpenRequest.hpp','editor/editing/include/lux/engine/editor/AssetSave.hpp','editor/editing/include/lux/engine/editor/CloseRequest.hpp','editor/editing/include/lux/engine/editor/editing/EditHistoryTarget.hpp','editor/activities/project/sinclude/lux/engine/editor/detail/ProjectWrite.hpp']
expired += [f for f in subprocess.check_output(['git','ls-files','editor/editing/pinclude'],cwd=r,text=True).splitlines() if 'LegacyPersistence' in f]
subprocess.run(['git','rm','--',*expired],cwd=r,check=True)
p=r/'editor/CMakeLists.txt';s=p.read_text();a=s.index('# Existing product only:');b=s.index('# All cross-layer',a);s=s[:a]+s[b:];s=s.replace('add_subdirectory(assets) # Remaining P12 save-adapter tests.\n','');p.write_text(s)
# Removing tracked legacy files in the isolated checkout never touches the original user's workspace.
subprocess.run(['git','rm','-r','--',*[f'editor/{x}' for x in roots]],cwd=r,check=True,stdout=subprocess.DEVNULL)
(w/'g-moves.json').write_text(json.dumps(moves,indent=2)+'\n')
