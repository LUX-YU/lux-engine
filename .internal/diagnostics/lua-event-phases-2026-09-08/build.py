"""Build one diagnostic TU with the real benchmark's qualified compile/link inputs.
No CMake inputs, production objects, DLLs, or installed headers are modified.
Run inside the existing MSVC developer environment.
"""
import ctypes, hashlib, json, re, subprocess, sys
from pathlib import Path

here = Path(__file__).resolve().parent
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
build = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w/d')
source = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-region-opt/final-source')
itt = Path('D:/Softwares/Intel/oneAPI/vtune/2026.3')
ninja = 'D:/Softwares/ninja-win/ninja.exe'

def split(command):
    fn = ctypes.windll.shell32.CommandLineToArgvW
    fn.restype = ctypes.POINTER(ctypes.c_wchar_p)
    count = ctypes.c_int()
    pointer = fn(command, ctypes.byref(count))
    result = [pointer[i] for i in range(count.value)]
    ctypes.windll.kernel32.LocalFree(pointer)
    return result

def run(command, name):
    base = name
    attempt = 1
    while (out/(name+'.log')).exists():
        attempt += 1
        name = base+'-'+str(attempt)
    (out/(name+'.command.json')).write_text(json.dumps(command, indent=2))
    with (out/(name+'.log')).open('wb') as log:
        code = subprocess.call(command, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    print(name, code, flush=True)
    if code: sys.exit(code)

entry = next(x for x in json.load(open(build/'compile_commands.json'))
             if x['file'].endswith('/script_runtime_benchmark.cpp'))
args = split(entry['command'])
old_obj = next(x[3:] for x in args if x.startswith('/Fo'))
args = [x for x in args if not x.startswith(('/Fo', '/Fd')) and not x.endswith('script_runtime_benchmark.cpp')]
args += ['/I'+str(Path(entry['file']).parent), '/I'+str(itt/'include'),
         '/Fo'+str(out/'probe.obj'), '/Fd'+str(out/'compile.pdb'), str(here/'probe.cpp')]
run(args, 'compile')
command = subprocess.check_output([ninja, '-C', str(build), '-t', 'commands',
                                   'bin/script_runtime_benchmark.exe'], text=True).splitlines()[-1]
link_part = command.split(' -- ', 1)[1].split(' && ', 1)[0]
args = split(link_part)
args = [str(out/'probe.obj') if x == old_obj else x for x in args]
replacements = {'/out:':out/'lua_event_phase_probe.exe', '/implib:':out/'probe.lib', '/pdb:':out/'probe.pdb'}
args = [next((prefix+str(path) for prefix,path in replacements.items()
              if x.lower().startswith(prefix)),x) for x in args]
args += [str(itt/'lib64/libittnotify.lib')]
run(args, 'link')
manifest = {'qualified_source':subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip(),
            'compile_entry':entry, 'generated_objects_reused':True, 'diagnostic_compile_does_not_modify_production_dlls':True,
            'files':[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
                     for p in (here/'probe.cpp', here/'build.py',out/'lua_event_phase_probe.exe',out/'probe.pdb')]}
(out/'probe-identity.json').write_text(json.dumps(manifest,indent=2))
