"""Bounded source/consumer inventory; raw hits accompany reviewed migration decisions."""
import csv, hashlib, json, re, subprocess, sys
from pathlib import Path

source=Path(__file__).resolve().parents[3]
root=Path(sys.argv[1]).resolve()
pattern=r'LUA54|LUAJIT|LUX_LUA_VM|LUA_VERSION_NUM|lua_newstate|lua_gc\(|lua_resume\(|lua_resetthread|luaL_newstate|luaL_openlibs|lua_setallocf|lua_setfenv|lua_setupvalue|lua_dump|lua_load'
raw=subprocess.check_output(['rg','-n',pattern,'CMakeLists.txt','cmake','modules','engine','test',
    '-g','!*.md','-g','!*.json','-g','!*.csv','-g','!*.log'],cwd=source,text=True)
(root/'lua-api-inventory.txt').write_text(raw)
groups={}
for line in raw.splitlines():
    name, number, text=line.split(':',2)
    groups.setdefault(name.replace('\\','/'),[]).append(dict(line=int(number),text=text.strip()))
entries=[]
for name,hits in groups.items():
    category=('installed-consumer' if 'installed-consumers/' in name else
              'qualification-script' if name.endswith(('.ps1','.py','.cmake')) else
              'generated-template' if '/cmake/' in name else 'source-consumer')
    reason=('Migrate allocator signature and exercise real installed API' if 'lua_newstate' in str(hits) else
            'Keep explicit historical VM selection, not current default qualification' if name.endswith('.ps1') else
            'Audit selected provider, C API signatures and version guards; preserve supported contracts')
    entries.append(dict(file=name,owner=name.rsplit('/',1)[0],category=category,reason=reason,hits=hits,
                        coverage='Pending stage result; a search hit is not a passing test'))
for name in ['engine/toolchain/lua/test/lua_runtime_benchmark_fixture.lua',
             'engine/toolchain/lua/test/lua_runtime_benchmark_fixture.symbols.json',
             'engine/toolchain/lua/test/lua_portability_fixture.lua',
             'engine/toolchain/lua/cmake/lux_package_lua_script.cmake']:
    entries.append(dict(file=name,owner='Lua Toolchain',category='cooked-artifact-input',
        reason='Recook with selected toolchain; preserve symbol and wire contracts',
        sha256=hashlib.sha256((source/name).read_bytes()).hexdigest(),
        test='run.py smoke / scene_script_lua_runtime_* / lua_script_packager_contract_test'))
for path in sorted((source/'cmake/installed-consumers').iterdir()):
    if path.is_dir() and ('script' in path.name or 'flowforge' in path.name or 'event-await' in path.name):
        entries.append(dict(file=str(path.relative_to(source)),owner='Installed SDK consumer',
            category='installed-consumer',reason='Resolve only selected installed SDK and VM; no old generated headers',
            test='RunScriptV3Consumers.ps1 named consumer; results pending'))
(root/'consumer_inventory.json').write_text(json.dumps(entries,indent=2))
mapping=[
 ('yield and resume result base; retained thread identity','engine/domain/simulation/scripting/lua/test/lua_vm_coroutine_contract_test.cpp','simulation_script_lua_vm_coroutine_contract_test'),
 ('scalar boundaries; async ability; event; cancellation; capacity','engine/domain/simulation/scripting/lua/test/lua_coroutine_integration_test.cpp','simulation_script_lua_coroutine_integration_test'),
 ('raw shape; const construction; reverse cleanup; OOM recovery','modules/function/script/lua/test/lua_value_test.cpp','simulation_script_lua_value_boundary_test'),
 ('provider qualification and recovery; lifecycle','engine/domain/simulation/scripting/lua/test/lua_value_runtime_test.cpp','simulation_script_lua_value_runtime_test'),
 ('prototype closure provenance and environment','engine/domain/simulation/scripting/lua/test/lua_closure_provenance_test.cpp','simulation_script_lua_closure_provenance_test'),
 ('prepared storage reuse and capacity','engine/domain/simulation/scripting/lua/test/lua_prepared_storage_test.cpp','simulation_script_lua_prepared_storage_test'),
 ('Scene assets and per-step portability protocol','engine/scene/integration/script/test/scene_script_lua_runtime_test.cpp','scene_script_lua_runtime_test and actual CTest variants'),
 ('packaged artifact contracts','engine/toolchain/lua/test/lua_script_packager_contract_test.cpp','lua_script_packager_contract_test'),
 ('real installed Ability provider and cleanup','cmake/installed-consumers/script-lua-values/Runtime.cpp','script-lua-values'),
 ('new per-instance payload readback','engine/domain/simulation/builtin/script/benchmark/script_runtime_benchmark.cpp','run.py smoke and compare.py scene-lua-event business oracle')]
with (root/'old_assertion_map.csv').open('w',newline='') as stream:
    writer=csv.writer(stream);writer.writerow(['assertion_family','source','real_entry','status'])
    for row in mapping:writer.writerow([*row,'Retained; see stage CTest/consumer raw logs for actual result'])
print(len(entries),'inventory entries;',len(mapping),'assertion families')
