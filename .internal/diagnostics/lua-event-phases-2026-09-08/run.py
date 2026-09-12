"""Serial timing, accounting and VTune ROI runs. Never run alongside a build/test."""
import csv, datetime, hashlib, json, os, re, subprocess, sys, time
from pathlib import Path

root = Path(sys.argv[1]).resolve()
stage = sys.argv[2]
build = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w/d')
source = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-region-opt/final-source')
artifact = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w/t/engine/toolchain/lua/lua_runtime_benchmark_fixture.lxsa')
vtune = Path('D:/Softwares/Intel/oneAPI/vtune/2026.3/bin64/vtune.exe')
probe = root/'lua_event_phase_probe.exe'
env = dict(os.environ)
env['PATH'] = ';'.join([str(build/'bin'), 'D:/Development/vcpkg/installed/x64-windows/bin',
                       *[x for x in env['PATH'].split(';') if 'CodeRepos' not in x and 'vcpkg' not in x]])
records = json.load(open(root/'runs.json')) if (root/'runs.json').exists() else []

def execute(args, name, extra=None):
    assert not (root/(name+'.log')).exists(), name
    start = time.monotonic()
    with (root/(name+'.log')).open('wb') as log:
        log.write((json.dumps(args)+'\n').encode()); log.flush()
        code = subprocess.call(args, stdout=log, stderr=subprocess.STDOUT, env={**env, **(extra or {})})
    item = dict(name=name,command=args,exit=code,seconds=time.monotonic()-start,
                utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    records.append(item)
    (root/'runs.json').write_text(json.dumps(records,indent=2))
    print(name,code,round(item['seconds'],2),flush=True)
    return item

def app(name, executable, frames, warmups=1000, size=10000, memory=False):
    return [str(executable),'--group','scene-lua-event','--mode','performance','--size',str(size),
            '--warmups',str(warmups),'--frames',str(frames),'--seed','1592598566',
            '--resume-budget','10000','--vm-accounting','on' if memory else 'off',
            '--lua-artifact',str(artifact),'--output',str(root/(name+'.csv'))]

def validate(item, phases, frames, size):
    name=item['name']
    text=(root/(name+'.log')).read_text(encoding='utf-8-sig')
    application_log=root/(name+'.application.log')
    if application_log.exists(): text+='\n'+application_log.read_text(encoding='utf-8-sig')
    rows=list(csv.DictReader((root/(name+'.csv')).open(encoding='utf-8-sig'))) if (root/(name+'.csv')).exists() else []
    errors=re.findall(r'invocation_errors=(\d+)',text)
    ok=item['exit']==0 and len(rows)==frames*(4 if phases else 1)
    ok=ok and len(errors)>=2 and all(int(v)==0 for v in errors) and 'shutdown' in text
    if phases:
        ok=ok and f'PHASE_INTEGRITY calls={size*frames} resumes={size*frames}' in text
        for field,phase in [('new_calls','register'),('resumes','resume_cleanup'),('waiter_visits','deliver'),
                            ('threads_created','register'),('threads_released','resume_cleanup')]:
            ok=ok and sum(int(r[field]) for r in rows if r['phase']==phase)==size*frames
    item['valid']=bool(ok)
    item['exe_sha256']=hashlib.sha256(Path(item['application'][0]).read_bytes()).hexdigest()
    (root/'runs.json').write_text(json.dumps(records,indent=2))
    if not ok: raise RuntimeError('Invalid '+name)

if stage=='smoke':
    args=app('smoke',probe,20,5,64,True)
    item=execute(args,'smoke');item['application']=args;validate(item,True,20,64)
elif stage=='timing':
    for pair in range(5):
        for mode in (['original','phases'] if pair%2==0 else ['phases','original']):
            name=f'timing-{pair}-{mode}'
            if any(x['name']==name and x.get('valid') for x in records):continue
            args=app(name,probe if mode=='phases' else build/'bin/script_runtime_benchmark.exe',2000)
            item=execute(args,name);item['application']=args;validate(item,mode=='phases',2000,10000)
elif stage=='memory':
    for i in range(2):
        name=f'memory-{i}'
        args=app(name,probe,500,memory=True)
        item=execute(args,name);item['application']=args;validate(item,True,500,10000)
elif stage=='hardware':
    args=app('hardware',probe,100,10,10000)
    item=execute([str(vtune),'-collect','hotspots','-knob','sampling-mode=hw','-start-paused',
                  '-return-app-exitcode','-result-dir',str(root/'hardware'), '--',*args],
                 'hardware',{'LUX_DIAG_ITT':'1'})
    item['role']='Capability only; failure is retained, no environment changes'
    (root/'runs.json').write_text(json.dumps(records,indent=2))
elif stage=='profile':
    for i in range(1,3):
        name=f'roi-{i}'
        if any(x['name']==name and x.get('valid') for x in records):continue
        args=app(name,probe,5000)
        command=[str(vtune),'-collect','hotspots','-knob','sampling-mode=sw',
                 '-knob','enable-stack-collection=true','-knob','enable-characterization-insights=false',
                 '-start-paused','-return-app-exitcode','-result-dir',str(root/name),
                 '-search-dir',str(root),'-search-dir',str(build/'bin'),
                 '-search-dir','D:/Development/vcpkg/installed/x64-windows/bin',
                 '-source-search-dir',str(source),
                 '-strategy','python.exe:notrace:trace,lua_event_phase_probe.exe:trace:notrace',
                 '--',sys.executable,'-B',str(Path(__file__).parent/'launch.py'),
                 str(root/(name+'.application.log')),*args]
        item=execute(command,name,{'LUX_DIAG_ITT':'1'});item['application']=args
        validate(item,True,5000,10000)
elif stage=='reports':
    for i in range(1,3):
        name=f'roi-{i}'
        for report,extra in [('summary',[]),('hotspots',[]),('physical',['-inline-mode','off']),
                             ('top-down',[]),('callstacks',[]),('modules',['-group-by','module']),
                             ('tasks',['-group-by','task'])]:
            label=name+'-'+report
            if (root/(label+'.log')).exists():continue
            command=[str(vtune),'-report','hotspots' if report in ('physical','modules','tasks') else report,
                     '-r',str(root/name),'-format','csv','-csv-delimiter','comma','-limit','10000',
                     '-report-output',str(root/(label+'.csv')),*extra]
            item=execute(command,label)
            if item['exit']:print('REPORT LIMITATION',label,flush=True)
elif stage=='phase-reports':
    for i in range(1,3):
        for phase in ['register','deliver','resume_cleanup']:
            for report in ['top-down','hotspots']:
                name=f'roi-{i}-{phase}-{report}'
                if (root/(name+'.log')).exists():continue
                command=[str(vtune),'-report',report,'-r',str(root/f'roi-{i}'),
                         '-filter','task='+phase,'-format','csv','-csv-delimiter','comma',
                         '-limit','10000','-report-output',str(root/(name+'.csv'))]
                item=execute(command,name)
                if item['exit']:raise RuntimeError(name)
elif stage=='assembly':
    for label, function in [
        ('active','lux::simulation::script::detail::ScriptInstances::active'),
        ('wait','lux::simulation::script::detail::ScriptExecution::waitEvent'),
        ('take','lux::simulation::script::detail::ScriptExecution::takeAwaitable'),
        ('resume','lux::simulation::script::detail::ScriptExecution::resumeOne'),
        ('value-move','lux::simulation::script::ScriptOwnedResumeValue::ScriptOwnedResumeValue'),
        ('newthread','lua_newthread')]:
        name='assembly-'+label
        if (root/(name+'.log')).exists():continue
        item=execute([str(vtune),'-report','hotspots','-r',str(root/'roi-1'),
                      '-source-object','function='+function,'-group-by','address',
                      '-format','csv','-csv-delimiter','comma','-limit','10000',
                      '-report-output',str(root/(name+'.csv'))],name)
        if item['exit']:print('ASSEMBLY LIMITATION',label,flush=True)
else:
    raise ValueError(stage)
