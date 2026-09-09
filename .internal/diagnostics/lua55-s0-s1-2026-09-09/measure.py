"""Fixed-workload independent processes; timing, allocation and sampling stay separate."""
import csv, datetime, hashlib, json, os, shutil, statistics, subprocess, sys, time
from pathlib import Path

root, label, slot = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
mode = sys.argv[4] if len(sys.argv) > 4 else 'aa'
build = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w')/slot
out = root/label/slot
out.mkdir(parents=True, exist_ok=True)
artifact = build.parent/'t/engine/toolchain/lua/lua_runtime_benchmark_fixture.lxsa'
exe = build/'bin/script_runtime_benchmark.exe'
env = dict(os.environ)
env['PATH'] = ';'.join([str(build/'bin'), 'D:/Development/vcpkg/installed/x64-windows/bin',
    *[p for p in env['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]])

def sha(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()

source = next(line.split('=',1)[1] for line in (build/'CMakeCache.txt').read_text().splitlines()
              if line.startswith('CMAKE_HOME_DIRECTORY:'))
identity = dict(source=source, commit=subprocess.check_output(['git','-C',source,'rev-parse','HEAD'],text=True).strip(),
    vm=slot, exe_sha256=sha(exe), artifact_sha256=sha(artifact), images={p.name:sha(p) for p in (build/'bin').glob('*.dll')})
(out/'identity.json').write_text(json.dumps(identity, indent=2))
results=[]
for pair in range(5 if mode == 'aa' else 1):
    for side in (['a','b'] if pair%2 == 0 else ['b','a']) if mode == 'aa' else ['memory']:
        name=f'{mode}-{pair}-{side}'
        log, data = out/(name+'.log'), out/(name+'.csv')
        if log.exists(): raise RuntimeError('Refusing overwrite '+str(log))
        command=[str(exe),'--group','scene-lua-event','--mode','performance', '--size','10000',
                 '--warmups','1000','--frames','2000','--seed','1592598566','--resume-budget','10000',
                 '--vm-accounting','on' if mode == 'memory' else 'off','--lua-artifact',str(artifact),
                 '--output',str(data)]
        start=time.monotonic()
        with log.open('wb') as stream:
            code=subprocess.call(command,stdout=stream,stderr=subprocess.STDOUT,env=env)
        text=log.read_text()
        rows=list(csv.DictReader(data.open())) if data.exists() else []
        valid=code==0 and len(rows)==2000 and (
            'BUSINESS_ORACLE,lua-event,instances=10000,cycles=3000,completed=30000000,'
            'per_instance=93001,checksum=930010000' in text)
        valid=valid and text.count('invocation_errors=0')==2 and 'lua-event,shutdown' in text
        valid=valid and all(int(r['continuations'])==int(r['awaitables'])==int(r['event_waiters'])==
                            int(r['queue_depth'])==0 for r in rows)
        ns=sorted(int(r['nanoseconds']) for r in rows)
        item=dict(name=name,pair=pair,side=side,command=command,exit=code,valid=valid,
                  wall_seconds=time.monotonic()-start,utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  total_ns=sum(ns), measured_operations=20000000, ns_per_operation=sum(ns)/20000000,
                  p50_batch_ns=statistics.median(ns) if ns else None,
                  p95_batch_ns=ns[int(len(ns)*.95)] if ns else None,p99_batch_ns=ns[int(len(ns)*.99)] if ns else None,
                  errors_observed=0 if valid else None, backlog=0 if valid else None)
        results.append(item)
        (out/'runs.json').write_text(json.dumps(results,indent=2))
        print(name, code, valid, round(item['ns_per_operation'],2),flush=True)
        if not valid: raise RuntimeError(name)
if mode == 'aa':
    deltas=[]
    for pair in range(5):
        values={r['side']:r['total_ns'] for r in results if r['pair']==pair}
        deltas.append(100*(values['b']/values['a']-1))
    passed=statistics.median(map(abs,deltas))<=3 and max(map(abs,deltas))<=8
    result=dict(deltas_percent=deltas, median_abs=statistics.median(map(abs,deltas)),max_abs=max(map(abs,deltas)),pass_aa=passed)
    (out/'aa-result.json').write_text(json.dumps(result,indent=2)); print(result,flush=True)
