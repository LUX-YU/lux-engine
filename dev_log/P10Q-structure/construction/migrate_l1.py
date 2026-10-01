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

contract=read('editor/contracts/CMakeLists.txt')
history=read('editor/editing/history/CMakeLists.txt').replace('SOURCE_FILES src/', 'SOURCE_FILES src/history/').replace(' src/EditHistory.cpp',' src/history/EditHistory.cpp')
sessions=read('editor/editing/sessions/CMakeLists.txt')
sessions=re.sub(r'\bsrc/', 'src/sessions/',sessions)
sessions=re.sub(r'\btest/', 'test/sessions/',sessions)
legacy=read('editor/editing/CMakeLists.txt').replace('add_subdirectory(history)\nadd_subdirectory(sessions)\n','')
view=read('editor/contracts/include/lux/engine/editor/views/ViewInfo.hpp')
marker=view.index('    enum class EViewError')
errors='#pragma once\n#include <lux/engine/editor/views/ViewInfo.hpp>\n#include <lux/cxx/compile_time/expected.hpp>\n\nnamespace lux::editor::views\n{\n'+view[marker:]
view=view[:marker]+'}\n'
view=view.replace('#include <lux/cxx/compile_time/expected.hpp>\n','')
cmakes=[]
for item in plan:
 if item['batch']!='L1':continue
 a,b=item['source'],item['destination']
 if a==b:continue
 if a.endswith(('CMakeLists.txt','README.md')) and a.startswith(('editor/contracts/','editor/editing/history/','editor/editing/sessions/','editor/workspace/README')):
  (repo/a).unlink();continue
 move(a,b)
write('editor/editing/CMakeLists.txt',contract+'\n'+history+'\n'+sessions+'\n# Existing product adaptation only; removed with its consumers at P12.\n'+legacy)
write('editor/editing/include/lux/engine/editor/views/ViewInfo.hpp',view)
write('editor/workbench/desktop/include/lux/engine/editor/views/ViewError.hpp',errors)
# The existing view_api is the sole provider even before its physical L3 move.
p='editor/views/api/CMakeLists.txt';s=read(p).replace('BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include',
 'BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include\n    ${PROJECT_SOURCE_DIR}/editor/workbench/desktop/include')
write(p,s)
for p in repo.joinpath('editor').rglob('*'):
 if p.suffix not in ('.hpp','.cpp','.h'):continue
 s=p.read_text(encoding='utf-8-sig')
 if p.name=='ViewInfo.hpp' or p.name=='ViewError.hpp':continue
 if '#include <lux/engine/editor/views/ViewInfo.hpp>' in s and re.search(r'\b(EViewError|ViewResult|ViewCloseFailure|ViewCloseResult)\b',s):
  p.write_text(s.replace('#include <lux/engine/editor/views/ViewInfo.hpp>', '#include <lux/engine/editor/views/ViewError.hpp>'),newline='\n')
write('editor/authoring/CMakeLists.txt','\n'.join('add_subdirectory('+x+')' for x in ['scene','material','flow','project','layout'])+'\n')
p='editor/CMakeLists.txt';s=read(p)
s=s.replace('add_subdirectory(contracts)\n','')
for old in ['tools/scene/model','tools/material/model','tools/flowforge/model','project','workspace/layout']:
 s=s.replace('add_subdirectory('+old+')\n','')
s=s.replace('add_subdirectory(editing)\n','add_subdirectory(editing)\nadd_subdirectory(authoring)\n')
write(p,s)
p='editor/authoring/layout/CMakeLists.txt';write(p,read(p).replace('    ${CMAKE_CURRENT_SOURCE_DIR}/../recovery/include INSTALL_TIME include)', '    INSTALL_TIME include)'))
# Relocate real paths in active build/test consumers only. Historical snapshots remain untouched.
mapping={
 'editor/editing/history/include':'editor/editing/include',
 'editor/editing/history/src':'editor/editing/src/history',
 'editor/editing/sessions/include':'editor/editing/include',
 'editor/editing/sessions/src':'editor/editing/src/sessions',
 'editor/editing/sessions/test':'editor/editing/test/sessions',
 'editor/contracts/include':'editor/editing/include',
 'editor/editing/history':'editor/editing',
 'editor/editing/sessions':'editor/editing',
 'editor/contracts':'editor/editing',
 'editor/tools/scene/model':'editor/authoring/scene',
 'editor/tools/material/model':'editor/authoring/material',
 'editor/tools/flowforge/model':'editor/authoring/flow',
 'editor/workspace/recovery/include':'editor/authoring/layout/include',
 'editor/workspace/layout':'editor/authoring/layout',
 'editor/project/src':'editor/authoring/project/src',
 'editor/project/include':'editor/authoring/project/include',
}
tracked=subprocess.check_output(['git','ls-files','-z'],cwd=repo).decode().split('\0')
tracked=sorted(set(tracked)|{x['destination'] for x in plan if x['batch']=='L1'})
for name in tracked:
 p=repo/name
 if not p.is_file() or name.startswith(('dev_log/','.internal/')):continue
 if p.suffix not in ('.txt','.cmake','.json','.py','.md','.hpp','.cpp'):continue
 s=p.read_text(encoding='utf-8-sig');new=s
 for a,b in mapping.items():new=new.replace(a,b)
 if new!=s:write(name,new)
# Rules' source directory identities also change when no trailing src/include was involved.
p='editor/tests/architecture/rules.json';rules=json.loads(read(p))
def remap(value):
 if isinstance(value,dict):return {k:remap(v) for k,v in value.items()}
 if isinstance(value,list):return [remap(x) for x in value]
 if value=='editor/project':return 'editor/authoring/project'
 return value
rules=remap(rules)
reused={'F003','F004','F005'}
rules['relocated_pure_definitions']=[{**x,'reason':'Original P01 body remains deleted; P10Q relocates the sole qualified pure definition here.'}
 for x in rules['expired_paths'] if x.get('id') in reused]
rules['expired_paths']=[x for x in rules['expired_paths'] if x.get('id') not in reused]
# Co-located targets do not own every sibling public/source file. Record exact foundation providers.
for target,policy in rules['foundation_targets'].items():
 original={'editor_contracts':'editor/contracts/','edit_history':'editor/editing/history/','edit_sessions':'editor/editing/sessions/'}[target]
 policy['files']=[x['destination'] for x in plan if x['source'].startswith(original) and x['source'].endswith(('.hpp','.cpp','.h'))]
 if target=='editor_contracts':
  policy['files'].append('editor/editing/include/lux/engine/editor/EditorError.hpp')
  policy['public_headers'].append('lux/engine/editor/EditorError.hpp')
rules['foundation_standard_headers']=sorted(set(rules['foundation_standard_headers'])|{'any','string_view'})
# Scene authoring cannot import runtime scheduling through scene_asset any longer.
for name in ['process_world_loading','process_execution','async','task']:
 rules['scene_model']['closure'].pop(name,None)
write(p,json.dumps(rules,ensure_ascii=False,indent=2)+'\n')
p='editor/tests/architecture/check_editor_boundaries.py';s=read(p)
s=s.replace('if not path.startswith(scope):\n                continue\n            for match',
 '''if "files" in policy:
                if path not in policy["files"]:
                    continue
            elif not path.startswith(scope):
                continue
            for match''')
s=s.replace('        scope = definitions[name]["path"] + "/"', '''        scope = definitions[name]["path"] + "/"
        owned = set(policy.get("files", []))
        node = targets.get(name, {})
        for item in filter(None, node.get("SOURCES", "").split(";")):
            path = Path(item)
            if not path.is_absolute():
                path = Path(node["SOURCE_DIR"]) / path
            if path.resolve().is_relative_to(repo.resolve()):
                owned.add(path.resolve().relative_to(repo.resolve()).as_posix())''')
s=s.replace('if path not in policy["files"]:', 'if path not in owned:')
s=s.replace('path.startswith("editor/editing/") and re.search',
 'path in rules["foundation_targets"]["edit_history"]["files"] and re.search')
write(p,s)
# Foundation fixtures now co-locate real providers too; no file can silently overwrite another target.
p='editor/tests/architecture/test_editor_boundaries.py';s=read(p)
s=s.replace('''                (directory / "CMakeLists.txt").write_text(
                    f"add_library({target} INTERFACE)\\nadd_library(fixture::{target} ALIAS {target})\\n",
                    encoding="utf-8")''', '''                cmake_file = directory / "CMakeLists.txt"
                previous = cmake_file.read_text() if cmake_file.exists() else ""
                cmake_file.write_text(previous +
                    f"add_library({target} INTERFACE)\\nadd_library(fixture::{target} ALIAS {target})\\n",
                    encoding="utf-8")''')
s=s.replace('for p in locations.values())', 'for p in dict.fromkeys(locations.values()))')
s=s.replace('history = root / "editor/editing/probe.hpp"', 'history = root / "editor/editing/include/lux/engine/editor/editing/EditHistory.hpp"')
s=s.replace('''            header.write_text(f"#include <{include}>\\n" if include else "", encoding="utf-8")''',
 '''            header.write_text(f"#include <{include}>\\n" if include else "", encoding="utf-8")
            cmake.write_text(cmake.read_text() + f'target_sources({source} INTERFACE "${{CMAKE_CURRENT_SOURCE_DIR}}/probe.hpp")\\n', encoding="utf-8")''')
write(p,s)
write('editor/editing/README.md','''# Editing

The shared `editor_contracts`, `edit_history` and `edit_sessions` targets own the pure identities,
history algorithm, SessionStore and SessionState. They share one physical public include root,
but retain their binary identity owners and separate installed packages.

ViewInfo is a pure observation; window errors and close preparation belong to view_api.
EditorError is a pure value supplied by editor_contracts.

The `editor_editing` target, EditHistoryTarget and transition/LegacyPersistenceState are retained
only for the existing product until P12. They are not dependencies of the new authoring or activities.
''')
write('editor/authoring/README.md','''# Authoring

Scene, Material, Flow, Project and Layout contain pure author values, validation and codecs.
The three sessions reuse Editing's History and SessionState. Process, runtime instances, UI,
compiler and physical file publication are outside this layer.

ProjectBuilder produces ProjectBuildConfig; it does not create files. ProjectCatalogModel is the
single immutable catalog provider. RecoveryManifest and layout plans never open author content.
''')
print('L1 physical providers relocated; original CMake bodies merged and old files removed.')
