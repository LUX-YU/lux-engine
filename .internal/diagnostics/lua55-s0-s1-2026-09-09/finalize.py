"""Normalize final diagnostic records and freeze readiness before archiving; no new measurements."""
import ast, csv, datetime, hashlib, json, sys
from pathlib import Path
root=Path(sys.argv[1]).resolve()
drivers=Path(__file__).parent
for script in drivers.glob('*.py'): ast.parse(script.read_text(encoding='utf-8'),filename=str(script))
costs=json.loads((root/'cost-summary.json').read_text())
assert len(costs)==31 and sum(r['complete'] for r in costs)==29
memory=[]
for label in ['s0-jit','corrected-jit','s0-lua54','corrected-lua54','final-lua55']:
    record=json.loads((root/'memory'/label/'result.json').read_text())
    assert record['valid']
    delta=record['measured_interval_delta_excludes_first_batch']
    count=delta['vm_coroutine_creations']
    assert count==19990000 and delta['vm_coroutine_resumes']==delta['vm_coroutine_releases']==count
    memory.append(dict(label=label,source=record['identity']['commit'],measured_cycles=count,
        allocations_per_cycle=delta['vm_allocations']/count,bytes_requested_per_cycle=delta['vm_requested_bytes']/count,
        reallocation_delta=delta['vm_reallocations'],process_peaks=record['process_peaks'],
        vm_allocator_failures=None,final_live_bytes=None,
        scope='Tracker installed after VM construction; last row before oracle/shutdown; process peaks include dependency closure'))
profile=root/'profile55-captured'
assert json.loads((profile/'identity.json').read_text())['valid']
assert all(r['exit']==0 for r in json.loads((profile/'runs.json').read_text()))
modules=list(csv.DictReader((profile/'modules.csv').open()))
assert len(modules)>=7 and all(r['Module']!='python.exe' for r in modules)
total=sum(float(r['CPU Time']) for r in modules)
for row in modules: row['percent_of_filtered_cpu']=100*float(row['CPU Time'])/total
assert 'jmp         000000018001D6D0' in (profile/'vcruntime-longjmp-export.txt').read_text()
assert json.loads((root/'vm-close-error/result.json').read_text())['exit']==0
assert json.loads((root/'physics-empty-exit-diagnostic-fixed/result.json').read_text())['application_exit']==0
result=dict(memory=memory,profile=dict(filtered_cpu_seconds=total,modules=modules,
    crt_rva='0x1d6d0',identification='Internal longjmp restoration; exported longjmp at 0x19b50 jumps here',
    evidence=['vcruntime-exports.txt','vcruntime-longjmp-export.txt','vcruntime-setjmp-disassembly.txt',
              'lua55-imports.txt','lua55-protected-disassembly.txt','top-down.csv'],
    PMU=False,scope='Whole child lifetime, not previous phase ROI'),
    physics_original_empty_exit=dict(exit=1,cause=None,reproduced=False,
        original_group_complete=False,repeat_group_complete=True,debugger_exit=0))
(root/'diagnostic-summary.json').write_text(json.dumps(result,indent=2))
(root/'driver-identities.json').write_text(json.dumps([dict(name=f.name,sha256=hashlib.sha256(f.read_bytes()).hexdigest())
    for f in sorted(drivers.iterdir()) if f.is_file()],indent=2))
(root/'ready-to-archive.json').write_text(json.dumps(dict(
    utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),source='55dcb3fa85a9a2b7dcc87b178dd9ff76ba75c644',
    status='CORRECTNESS_QUALIFIED_COST_OPEN',serial_measurements_finished=True,
    unresolved=['Lua54 A/A','same-VM safety cost','Lua55 migration costs','one Physics empty-log exit']),indent=2))
print('FINAL RECORDS',len(memory),'memory variants;',len(modules),'profiled modules; 29 complete pairs groups; COST_OPEN')
