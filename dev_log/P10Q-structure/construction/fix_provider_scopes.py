from pathlib import Path
import json, subprocess

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
work=Path(__file__).resolve().parent
plan=json.loads((work/'file-plan.json').read_text())
baseline=json.loads(subprocess.check_output(['git','show','f7c27f9375cbf8dd8af37b30a6027a460de26213:editor/tests/architecture/rules.json'],cwd=repo))
file=repo/'editor/tests/architecture/rules.json'
rules=json.loads(file.read_text())
definitions={x['name']:x for x in baseline['targets']}
mapping={x['source']:x['destination'] if (repo/x['destination']).exists() else x['source'] for x in plan}

# Preserve precisely the old checked bodies; add newly declared target sources dynamically.
# Co-location must not silently grant one provider all siblings' APIs or lose its own private headers.
for name,original in baseline.items():
    if not isinstance(original,dict) or 'closure' not in original or name not in definitions:continue
    policy=rules[name]
    oldroot=definitions[name]['path']+'/'
    owned=original.get('files')
    if owned is None:
        nested=[v['path']+'/' for n,v in definitions.items() if n!=name and v['path'].startswith(oldroot)]
        owned=[x['source'] for x in plan if x['source'].startswith(oldroot)
               and not any(x['source'].startswith(p) for p in nested)
               and x['source'].endswith(('.h','.hpp','.cpp','.cc'))
               and x['source'] not in original.get('exclude_files',[])]
    if owned:policy['files']=[mapping.get(x,x) for x in owned]
file.write_text(json.dumps(rules,ensure_ascii=False,indent=2)+'\n',newline='\n')

p=repo/'cmake/EditorArchitectureChecks.cmake'
s=p.read_text().replace('IMPORTED SOURCES INCLUDE_DIRECTORIES','IMPORTED SOURCES INTERFACE_SOURCES INCLUDE_DIRECTORIES')
p.write_text(s,newline='\n')
p=repo/'editor/tests/architecture/check_editor_boundaries.py';s=p.read_text()
if 'def owned_sources(' not in s:
    helper='''def owned_sources(repo, node, policy):
    owned = set(policy.get("files", []))
    for key in ("SOURCES", "INTERFACE_SOURCES"):
        for item in filter(None, node.get(key, "").split(";")):
            if "$<" in item:
                continue  # Resolved compiler dependencies are inspected by the layering qualification.
            path = Path(item)
            if not path.is_absolute():
                path = Path(node["SOURCE_DIR"]) / path
            if path.resolve().is_relative_to(repo.resolve()):
                owned.add(path.resolve().relative_to(repo.resolve()).as_posix())
    return owned


'''
    s=s.replace('def check_foundations(',helper+'def check_foundations(')
    a=s.index('        owned = set(policy.get("files", []))',s.index('def check_foundations'))
    b=s.index('        for path, source in sources.items():',a)
    s=s[:a]+'        owned = owned_sources(repo, targets.get(name, {}), policy)\n'+s[b:]
    s=s.replace('    scope = model_path + "/"\n','    scope = model_path + "/"\n    owned = owned_sources(repo, targets.get(name, {}), policy)\n')
    s=s.replace('("files" in policy and path not in policy["files"])','("files" in policy and path not in owned)')
p.write_text(s,newline='\n')

# Every negative include fixture is attached to its actual interface target, not merely placed nearby.
for p in (repo/'editor/tests/architecture').glob('test_*boundaries.py'):
    s=p.read_text()
    if p.name=='test_editor_boundaries.py':continue
    old='''                (folder / "dummy.cpp").write_text("int " + value + "_fixture;\\n")
                (folder / "CMakeLists.txt").write_text(f"add_library({value} STATIC dummy.cpp)\\nadd_library(fixture::{value} ALIAS {value})\\n")
                top += f"add_subdirectory({locations[value]})\\n"'''
    new='''                (folder / (value + ".cpp")).write_text("int " + value + "_fixture;\\n")
                cmake_file = folder / "CMakeLists.txt"
                previous = cmake_file.read_text() if cmake_file.exists() else ""
                cmake_file.write_text(previous + f"add_library({value} STATIC {value}.cpp)\\nadd_library(fixture::{value} ALIAS {value})\\n")
                entry = f"add_subdirectory({locations[value]})\\n"
                if entry not in top:
                    top += entry'''
    s=s.replace(old,new)
    static_marker='''            probe.write_text(f"#include <{header}>\\n" if header else "")'''
    if static_marker in s and 'for value in targets:' in s and 'target_sources({target} PRIVATE' not in s:
        s=s.replace(static_marker,static_marker+'''
            provider = root / locations[target] / "CMakeLists.txt"
            provider.write_text(provider.read_text() + f'target_sources({target} PRIVATE "{probe.as_posix()}")\\n')''')
    for variable in ['probe','header']:
        marker=f'            {variable}.write_text(f"#include <{{header}}>\\n" if header else "")'
        if marker in s and 'locations[model]' in s:
            extra='''
            provider = root / locations[model] / "CMakeLists.txt"
            provider.write_text(provider.read_text() + f'target_sources({model} INTERFACE "${{CMAKE_CURRENT_SOURCE_DIR}}/probe.hpp")\\n')'''
            if 'target_sources({model} INTERFACE' not in s:s=s.replace(marker,marker+extra)
    p.write_text(s,newline='\n')
print('Exact target provider scopes and interface sources recorded.')
