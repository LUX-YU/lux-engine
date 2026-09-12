"""Reject a mismatched SDK selection and a real header/runtime DLL mismatch without changing either SDK."""
import hashlib, json, os, shutil, subprocess, sys
from pathlib import Path

root=Path(sys.argv[1]).resolve()
out=root/'vm-negative'
out.mkdir(exist_ok=False)
results=[]
source=out/'source'
source.mkdir()
(source/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.22)
project(vm_mismatch LANGUAGES CXX)
include("${FIRST}/share/lux-engine-function/script_lua/LuxLuaVmSelection.cmake")
include("${SECOND}/share/lux-engine-function/script_lua/LuxLuaVmSelection.cmake")
message(FATAL_ERROR "MISMATCH WAS NOT REJECTED")
''')
prefix=Path('E:/SyncForder/CodeRepos/install/o/q')
for first,second in [('s1-lua55','s1-jit'),('s1-jit','s1-lua55')]:
    name=f'{first}-then-{second}'
    command=['cmake','-S',str(source),'-B',str(out/name),'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
        '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
        '-DCMAKE_PREFIX_PATH=E:/SyncForder/CodeRepos/install/o/lua55;D:/Development/vcpkg/installed/x64-windows',
        '-DFIRST='+str(prefix/first),'-DSECOND='+str(prefix/second)]
    with (out/(name+'.log')).open('wb') as stream:
        code=subprocess.call(command,stdout=stream,stderr=subprocess.STDOUT)
    valid=code!=0 and 'Lux Lua VM selection mismatch' in (out/(name+'.log')).read_text()
    results.append(dict(case=name,command=command,exit=code,valid=valid))
    (out/'results.json').write_text(json.dumps(results,indent=2))
    if not valid: raise RuntimeError(name)
wrong=out/'wrong-dll'
wrong.mkdir()
shutil.copy2(root/'dependencies/build-lua55/lua55_smoke.exe',wrong/'lua55_smoke.exe')
shutil.copy2('E:/SyncForder/CodeRepos/build/deps/lua54-vcpkg/x64-windows/bin/lua.dll',wrong/'lux_lua55.dll')
command=[str(wrong/'lua55_smoke.exe')]
with (out/'wrong-dll.log').open('wb') as stream:
    code=subprocess.call(command,stdout=stream,stderr=subprocess.STDOUT)
valid=code==13 and 'header/runtime version mismatch' in (out/'wrong-dll.log').read_text()
results.append(dict(case='header55-runtime54',command=command,exit=code,valid=valid,
    images={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in wrong.iterdir()}))
(out/'results.json').write_text(json.dumps(results,indent=2))
print('VM_NEGATIVE',len(results),all(r['valid'] for r in results),flush=True)
if not valid: raise RuntimeError('Runtime mismatch was not safely rejected')
