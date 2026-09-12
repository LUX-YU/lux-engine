"""Separate allocator accounting run, loaded-module identity, and sampled process memory."""
import csv, ctypes as ct, ctypes.wintypes as wt, hashlib, json, os, subprocess, sys, time
from pathlib import Path

root,label,image=Path(sys.argv[1]).resolve(),sys.argv[2],sys.argv[3]
path=root/'images'/image
out=root/'memory'/label
out.mkdir(parents=True,exist_ok=False)
identity=json.loads((path/'identity.json').read_text())
kernel=ct.WinDLL('kernel32',use_last_error=True)
psapi=ct.WinDLL('psapi',use_last_error=True)
kernel.GetCurrentProcess.restype=wt.HANDLE
kernel.SetProcessAffinityMask.argtypes=[wt.HANDLE,ct.c_size_t]
kernel.OpenProcess.argtypes=[wt.DWORD,wt.BOOL,wt.DWORD]
kernel.OpenProcess.restype=wt.HANDLE
kernel.CloseHandle.argtypes=[wt.HANDLE]
psapi.EnumProcessModulesEx.argtypes=[wt.HANDLE,ct.POINTER(wt.HMODULE),wt.DWORD,ct.POINTER(wt.DWORD),wt.DWORD]
psapi.GetModuleFileNameExW.argtypes=[wt.HANDLE,wt.HMODULE,wt.LPWSTR,wt.DWORD]
class Memory(ct.Structure):
    _fields_=[('cb',wt.DWORD),('PageFaultCount',wt.DWORD),
        *[(name,ct.c_size_t) for name in ['PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage',
            'QuotaPagedPoolUsage','QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage',
            'PagefileUsage','PeakPagefileUsage','PrivateUsage']]]
psapi.GetProcessMemoryInfo.argtypes=[wt.HANDLE,ct.POINTER(Memory),wt.DWORD]
assert kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(),16)
env=dict(os.environ)
env['PATH']=';'.join([str(path),'D:/Development/vcpkg/installed/x64-windows/bin',
    *[p for p in env['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]])
data=out/'event.csv'
command=[str(path/'script_runtime_benchmark.exe'),'--group','scene-lua-event','--mode','performance',
    '--size','10000','--warmups','1000','--frames','2000','--resume-budget','10000','--seed','1592598566',
    '--vm-accounting','on','--lua-artifact',str(path/'lua_runtime_benchmark_fixture.lxsa'),'--output',str(data)]
modules=set()
samples=[]
with (out/'event.log').open('wb') as stream:
    process=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT,env=env)
    handle=kernel.OpenProcess(0x410,False,process.pid)
    if not handle: raise ct.WinError(ct.get_last_error())
    try:
        while process.poll() is None:
            counters=Memory();counters.cb=ct.sizeof(counters)
            if psapi.GetProcessMemoryInfo(handle,ct.byref(counters),ct.sizeof(counters)):
                samples.append({name:getattr(counters,name) for name in ['PeakWorkingSetSize','WorkingSetSize',
                    'PagefileUsage','PeakPagefileUsage','PrivateUsage']})
            loaded=(wt.HMODULE*1024)();needed=wt.DWORD()
            if psapi.EnumProcessModulesEx(handle,loaded,ct.sizeof(loaded),ct.byref(needed),3):
                assert needed.value<=ct.sizeof(loaded)
                for module in loaded[:needed.value//ct.sizeof(wt.HMODULE)]:
                    name=ct.create_unicode_buffer(32768)
                    if psapi.GetModuleFileNameExW(handle,module,name,len(name)): modules.add(name.value)
            time.sleep(.25)
    finally: kernel.CloseHandle(handle)
rows=list(csv.DictReader(data.open())) if data.exists() else []
text=(out/'event.log').read_text()
valid=process.returncode==0 and len(rows)==2000 and 'completed=30000000,per_instance=93001,checksum=930010000' in text
valid=valid and text.count('invocation_errors=0')==2 and all(r['git_commit']==identity['commit'] for r in rows)
module_ids=[]
for name in sorted(modules):
    file=Path(name)
    if 'lua' in file.name.lower() or 'script' in file.name.lower():
        with file.open('rb') as stream: digest=hashlib.file_digest(stream,'sha256').hexdigest()
        module_ids.append(dict(path=str(file),sha256=digest))
vms=[m for m in module_ids if Path(m['path']).name.lower() in ['lua51.dll','lua.dll','lux_lua55.dll']]
valid=valid and len(vms)==1 and Path(vms[0]['path']).parent.samefile(path)
fields=['vm_allocations','vm_reallocations','vm_frees','vm_requested_bytes','vm_released_bytes',
    'vm_coroutine_creations','vm_coroutine_resumes','vm_coroutine_releases']
result=dict(command=command,identity=identity,exit=process.returncode,valid=valid,loaded_modules=module_ids,
    vm=rows[-1]['lua_vm'] if rows else None,version=rows[-1]['lua_version'] if rows else None,
    process_peaks={key:max(s[key] for s in samples) for key in samples[0]} if samples else {},
    sample_interval_seconds=.25,cumulative_last_row={key:int(rows[-1][key]) for key in fields} if rows else {},
    measured_interval_delta_excludes_first_batch={key:int(rows[-1][key])-int(rows[0][key]) for key in fields} if rows else {},
    measurement_scope='VM allocation diagnostics; timing excluded; process counters include complete dependency closure; '
        'last row precedes oracle/shutdown/VM destruction, not final-live or a whole-run zero-leak claim')
(out/'result.json').write_text(json.dumps(result,indent=2))
print(label,valid,result['vm'],result['process_peaks'],flush=True)
if not valid: raise RuntimeError('Invalid memory/identity run')
