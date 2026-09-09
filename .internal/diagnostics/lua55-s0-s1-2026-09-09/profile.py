"""One whole-process software Hotspots diagnostic. Never used as an uninstrumented timing trial."""
import csv, ctypes as ct, json, os, subprocess, sys, time
from pathlib import Path
root=Path(sys.argv[1]).resolve();out=root/'profile55-captured';out.mkdir(exist_ok=False)
image=root/'images/final-lua55/d'
vtune='D:/Softwares/Intel/oneAPI/vtune/2026.3/bin64/vtune.exe'
env=dict(os.environ)
env['PATH']=';'.join([str(image),'D:/Development/vcpkg/installed/x64-windows/bin',
    *[p for p in env['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]])
kernel=ct.WinDLL('kernel32');kernel.GetCurrentProcess.restype=ct.c_void_p
kernel.SetProcessAffinityMask.argtypes=[ct.c_void_p,ct.c_size_t]
assert kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(),16)
app=[str(image/'script_runtime_benchmark.exe'),'--group','scene-lua-event','--mode','performance',
    '--size','10000','--warmups','1000','--frames','2000','--resume-budget','10000','--seed','1592598566',
    '--vm-accounting','off','--lua-artifact',str(image/'lua_runtime_benchmark_fixture.lxsa'),
    '--output',str(out/'business.csv')]
command=[vtune,'-collect','hotspots','-knob','sampling-mode=sw','-knob','enable-stack-collection=true',
    '-knob','enable-characterization-insights=false','-return-app-exitcode','-result-dir',str(out/'result'),
    '-search-dir',str(image),'-source-search-dir','E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-region-opt/final-source',
    '-source-search-dir',str(root/'dependencies/qualified-lua-5.5.1'),'--',sys.executable,'-B',
    str(Path(__file__).with_name('profile-target.py')),str(out)]
(out/'target-command.json').write_text(json.dumps(app,indent=2))
records=[]
def run(name,args):
    start=time.monotonic()
    with (out/(name+'.log')).open('wb') as log:
        code=subprocess.call(args,stdout=log,stderr=subprocess.STDOUT,env=env)
    records.append(dict(name=name,command=args,exit=code,seconds=time.monotonic()-start))
    (out/'runs.json').write_text(json.dumps(records,indent=2))
    print('VTUNE',name,code,flush=True)
    return code
code=run('collect',command)
rows=list(csv.DictReader((out/'business.csv').open())) if (out/'business.csv').exists() else []
text=(out/'target.log').read_text() if (out/'target.log').exists() else ''
child=json.loads((out/'target-exit.json').read_text()) if (out/'target-exit.json').exists() else {}
valid=code==0 and child.get('exit')==0 and len(rows)==2000 and 'completed=30000000,per_instance=93001,checksum=930010000' in text
valid=valid and text.count('invocation_errors=0')==2
(out/'identity.json').write_text(json.dumps(dict(valid=valid,image=json.loads((image/'identity.json').read_text()),
    scope='Whole child process including warmup, setup, oracle and shutdown; launcher excluded by report process filter; not prior ROI shares',
    affinity=16,PMU=False,timing_acceptance=False),indent=2))
if not valid: raise RuntimeError('Invalid profile business work')
for name,extra in [('summary',[]),('hotspots',[]),('top-down',[]),('modules',['-group-by','module'])]:
    run(name,[vtune,'-report','hotspots' if name=='modules' else name,'-r',str(out/'result'),
        '-format','csv','-csv-delimiter','comma','-limit','10000',
        '-filter','process=script_runtime_benchmark.exe','-report-output',str(out/(name+'.csv')),*extra])
