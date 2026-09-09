"""Same-work Physics2D mixed-backend sentinel; no per-language business counter is inferred."""
import csv, ctypes as ct, hashlib, json, os, statistics, subprocess, sys
from pathlib import Path
root,label,left,right=Path(sys.argv[1]).resolve(),*sys.argv[2:5]
out=root/'costs'/label
out.mkdir(parents=True,exist_ok=False)
kernel=ct.WinDLL('kernel32');kernel.GetCurrentProcess.restype=ct.c_void_p
kernel.SetProcessAffinityMask.argtypes=[ct.c_void_p,ct.c_size_t]
assert kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(),16)
paths={side:root/'images'/name for side,name in [('a',left),('b',right)]}
identities={side:json.loads((path/'identity.json').read_text()) for side,path in paths.items()}
assert identities['a']['physics_fixtures']==identities['b']['physics_fixtures']
runs=[]
keys=['active_instances','physics_queries','script_calls','events','continuations','awaitables',
    'event_waiters','next_step_waits','queue_depth','checksum','resumes','suspensions']
for pair in range(5):
    for side in (['a','b'] if pair%2==0 else ['b','a']):
        path=paths[side]
        name=f'physics-{pair}-{side}'
        data=out/(name+'.csv')
        command=[str(path/'physics2d_script_benchmark.exe'),'--group','scene-physics-mixed',
            '--mode','performance','--size','10000','--warmups','300','--frames','2000',
            '--workers','0','--trace','off','--lua-policy','default',
            '--lua-artifact',str(path/'physics2d_lua_fixture.lxsa'),
            '--flowforge-artifact',str(path/'physics2d_flowforge_fixture.lxsa'),'--output',str(data)]
        env=dict(os.environ)
        env['PATH']=';'.join([str(path),'D:/Development/vcpkg/installed/x64-windows/bin',
            *[p for p in env['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]])
        with (out/(name+'.log')).open('wb') as stream:
            code=subprocess.call(command,stdout=stream,stderr=subprocess.STDOUT,env=env)
        rows=list(csv.DictReader(data.open())) if data.exists() else []
        valid=code==0 and len(rows)==2000 and all(r['git_commit']==identities[side]['commit'] and
            r['build_type']=='RelWithDebInfo' and r['size']=='10000' and r['active_instances']=='10000'
            and r['seed']==str(0x5EED2026) and r['workers']=='0' and r['trace']=='0' for r in rows)
        times=sorted(int(r['nanoseconds']) for r in rows)
        final={key:int(rows[-1][key]) for key in keys} if rows else {}
        valid=valid and final.get('physics_queries',0)>0 and final.get('resumes',0)>0
        runs.append(dict(name=name,pair=pair,side=side,command=command,exit=code,valid=valid,
            identity=identities[side],total_ns=sum(times),operations=len(rows),operation_unit='mixed-physics-frame',
            ns_per_frame=sum(times)/len(rows) if rows else None,
            p50_batch_ns=statistics.median(times) if rows else None,
            p95_batch_ns=times[1900] if rows else None,p99_batch_ns=times[1980] if rows else None,
            final_counters=final,errors_observed=None,
            error_scope='Existing harness checks retained failures after every frame; no cumulative error export, '
                'shutdown result ignored by original driver; checksum covers C++ only, Lua validates bool and payload'))
        (out/'runs.json').write_text(json.dumps(runs,indent=2))
        print(label,name,valid,round(sum(times)/len(rows),2) if rows else None,flush=True)
        if not valid: raise RuntimeError(name)
deltas=[]
for pair in range(5):
    values={r['side']:r for r in runs if r['pair']==pair}
    assert values['a']['final_counters']==values['b']['final_counters'],'Business mismatch'
    deltas.append(100*(values['b']['total_ns']/values['a']['total_ns']-1))
(out/'summary.json').write_text(json.dumps(dict(deltas_percent=deltas,median_percent=statistics.median(deltas),
    faster_pairs=sum(d<0 for d in deltas),cost_acceptance='NOT_GRANTED',affinity_mask=16),indent=2))
