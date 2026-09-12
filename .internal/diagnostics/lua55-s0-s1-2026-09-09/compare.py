"""Compare immutable runnable image sets; timings never include memory/profiler diagnostics."""
import csv, ctypes as ct, hashlib, json, os, statistics, subprocess, sys, time
from pathlib import Path

root, label, left, right = Path(sys.argv[1]).resolve(), *sys.argv[2:5]
families = sys.argv[5:] or ['event','update','ability','coroutine','sequence','cold8','cold8192','churn8','churn8192']
cases = {
    'event': ('scene-lua-event',10000,1000,2000,10000),
    'update': ('scene-lua-update-heavy',10000,1000,2000,10000),
    'ability': ('scene-lua-ability',10000,1000,2000,10000),
    'coroutine': ('scene-lua-coroutine',10000,1000,2000,10000),
    'sequence': ('scene-lua-sequence',10000,1000,2000,10000),
    # Existing population driver deliberately executes two complete populations, including a fresh VM.
    # CLI requires nonzero warmups; population driver ignores that option and performs no warmup.
    'cold8': ('scene-lua-population',8,1,2,8),
    'cold8192': ('scene-lua-population',8192,1,2,8192),
    'churn8': ('scene-lua-object-churn',8,16,128,8),
    'churn8192': ('scene-lua-object-churn',8192,16,128,8192),
}
kernel=ct.WinDLL('kernel32',use_last_error=True)
kernel.GetCurrentProcess.restype=ct.c_void_p
kernel.SetProcessAffinityMask.argtypes=[ct.c_void_p,ct.c_size_t]
if not kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(),16): raise ct.WinError(ct.get_last_error())
out=root/'costs'/label
out.mkdir(parents=True,exist_ok=False)
images={side:root/'images'/image for side,image in [('a',left),('b',right)]}
identities={side:json.loads((path/'identity.json').read_text()) for side,path in images.items()}
assert identities['a']['artifact_sha256']==identities['b']['artifact_sha256']
for side,path in images.items():
    for name,identity in identities[side]['images'].items():
        with (path/name).open('rb') as stream: actual=hashlib.file_digest(stream,'sha256').hexdigest()
        assert actual==identity['sha256'],name
(out/'identity.json').write_text(json.dumps(dict(images=identities,affinity_mask=16,
    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),cases=cases),indent=2))
runs=[]
summaries=[]
for family in families:
    group,size,warmups,frames,budget=cases[family]
    for pair in range(5):
        for side in (['a','b'] if pair%2==0 else ['b','a']):
            path=images[side]
            name=f'{family}-{pair}-{side}'
            log,data=out/(name+'.log'),out/(name+'.csv')
            command=[str(path/'script_runtime_benchmark.exe'),'--group',group,'--mode','performance',
                '--size',str(size),'--warmups',str(warmups),'--frames',str(frames),'--resume-budget',str(budget),
                '--seed','1592598566','--vm-accounting','off',
                '--lua-artifact',str(path/'lua_runtime_benchmark_fixture.lxsa'),'--output',str(data)]
            env=dict(os.environ)
            env['PATH']=';'.join([str(path),'D:/Development/vcpkg/installed/x64-windows/bin',
                *[p for p in env['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]])
            start=time.monotonic()
            with log.open('wb') as stream: code=subprocess.call(command,stdout=stream,stderr=subprocess.STDOUT,env=env)
            text=log.read_text()
            rows=list(csv.DictReader(data.open())) if data.exists() else []
            valid=code==0 and bool(rows) and all(r['git_commit']==identities[side]['commit'] and
                r['build_type']=='RelWithDebInfo' and int(r['size'])==size and r['seed']=='1592598566' for r in rows)
            if family in ['event','update','ability','coroutine']:
                valid=valid and len(rows)==frames and text.count('invocation_errors=0')==2
                valid=valid and all(int(r['continuations'])==int(r['awaitables'])==int(r['queue_depth'])==0 for r in rows)
                valid=valid and int(rows[-1]['calls'])==(warmups+frames)*size
            if family=='event':
                valid=valid and 'completed=30000000,per_instance=93001,checksum=930010000' in text
                valid=valid and all(int(r['event_waiters'])==0 for r in rows)
            if family in ['event','coroutine']:
                valid=valid and int(rows[-1]['resumes'])==(warmups+frames)*size
            if family=='ability': valid=valid and int(rows[-1]['ability_calls'])==2*(warmups+frames)*size
            if family.startswith('cold'):
                valid=valid and len(rows)==6 and sum(int(r['lifecycle_begins']) for r in rows)==2*size
                valid=valid and sum(int(r['lifecycle_ends']) for r in rows)==2*size
            if family=='sequence':
                valid=valid and len(rows)>=frames and int(rows[-1]['calls'])==int(rows[-1]['ability_calls'])
                valid=valid and int(rows[-1]['resumes'])==3*int(rows[-1]['calls'])
                valid=valid and int(rows[-1]['continuations'])==int(rows[-1]['awaitables'])==int(rows[-1]['queue_depth'])==0
            churn_count=max(1,min(100,size//10))
            if family.startswith('churn'):
                valid=valid and len(rows)==frames and int(rows[-1]['lifecycle_ends'])==(warmups+frames)*churn_count
                valid=valid and int(rows[-1]['lifecycle_begins'])==size+(warmups+frames)*churn_count
                valid=valid and all(int(r['active_instances'])==size for r in rows)
            ns=sorted(int(r['nanoseconds']) for r in rows)
            operations=2*size if family.startswith('cold') else (len(rows) if family=='sequence' else size*frames)
            unit='population-instance' if family.startswith('cold') else ('batch-including-drain' if family=='sequence'
                else 'complete-wait-cycle' if family=='event' else 'update')
            if family.startswith('churn'):
                operations=frames*churn_count
                unit='retire-rematerialize-instance'
            phases={}
            for row in rows: phases[row['scenario']]=phases.get(row['scenario'],0)+int(row['nanoseconds'])
            record=dict(name=name,family=family,pair=pair,side=side,command=command,exit=code,valid=valid,
                wall_seconds=time.monotonic()-start,total_ns=sum(ns),operation_unit=unit,operations=operations,
                ns_per_operation=sum(ns)/operations,p50_batch_ns=statistics.median(ns) if ns else None,
                p95_batch_ns=ns[min(len(ns)-1,int(len(ns)*.95))] if ns else None,
                p99_batch_ns=ns[min(len(ns)-1,int(len(ns)*.99))] if ns else None,phase_totals=phases,
                final_counters={k:rows[-1][k] for k in ['calls','ability_calls','resumes','checksum','continuations',
                    'awaitables','event_waiters','queue_depth']} if rows else {},
                errors_observed=0 if valid and text.count('invocation_errors=0')==2 else None,
                error_scope='runtime steady/shutdown' if 'INTEGRITY' in text else 'exit and existing assertions only')
            runs.append(record)
            (out/'runs.json').write_text(json.dumps(runs,indent=2))
            print(label,name,code,valid,round(record['ns_per_operation'],2),flush=True)
            if not valid: raise RuntimeError('Invalid trial '+name)
    deltas=[]
    for pair in range(5):
        values={r['side']:r for r in runs if r['family']==family and r['pair']==pair}
        assert values['a']['final_counters']==values['b']['final_counters'],'Mismatched business work'
        deltas.append(100*(values['b']['total_ns']/values['a']['total_ns']-1))
    summaries.append(dict(family=family,deltas_percent=deltas,median_percent=statistics.median(deltas),
        faster_pairs=sum(d<0 for d in deltas),cost_acceptance='NOT_GRANTED; Lua54 A/A failed; migration is not optimization'))
    (out/'summary.json').write_text(json.dumps(summaries,indent=2))
