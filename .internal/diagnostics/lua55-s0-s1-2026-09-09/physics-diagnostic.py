"""One same-work debugger run after an empty-log exit; never included in timing acceptance."""
import ctypes as ct, json, os, re, subprocess, sys
from pathlib import Path
root=Path(sys.argv[1]).resolve()
out=root/(sys.argv[2] if len(sys.argv)>2 else 'physics-empty-exit-diagnostic-fixed')
out.mkdir(exist_ok=False)
image=root/'images/final-lua55/d'
original=json.loads((root/'costs/physics-jit-to55/runs.json').read_text())[-1]
assert original['exit']==1 and not original['valid']
app=list(original['command'])
app[-1]=str(out/'business.csv')
environment=dict(os.environ)
environment['PATH']=';'.join([str(image),'D:/Development/vcpkg/installed/x64-windows/bin',
    *[p for p in environment['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]])
kernel=ct.WinDLL('kernel32');kernel.GetCurrentProcess.restype=ct.c_void_p
kernel.SetProcessAffinityMask.argtypes=[ct.c_void_p,ct.c_size_t]
assert kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(),16)
debugger='C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe'
commands='bu ntdll!RtlExitUserProcess ".echo EXITPROBE; r rcx; k 32; g"; g'
command=[debugger,'-hd','-G','-y',str(image),'-logo',str(out/'debugger.log'),'-c',commands,*app]
with (out/'console.log').open('wb') as log:
    code=subprocess.call(command,stdout=log,stderr=subprocess.STDOUT,env=environment)
text=(out/'debugger.log').read_text()
probe=re.search(r'EXITPROBE\s+rcx=([0-9a-fA-F]+)',text)
(out/'result.json').write_text(json.dumps(dict(command=command,debugger_exit=code,
    original=original['name'],original_exit=original['exit'],image=json.loads((image/'identity.json').read_text()),
    exit_probe_observed=probe is not None,application_exit=int(probe.group(1),16) if probe else None,
    timing_qualification=False,scope='Diagnostic only; -hd retains normal heap; debugger overhead excluded'),indent=2))
print('PHYSICS DEBUGGER EXIT',code,flush=True)
if code or probe is None: raise RuntimeError('No actual exit probe')
