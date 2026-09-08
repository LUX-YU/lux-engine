"""Keep unprofiled batch durations, VM accounting and sampled CPU attribution separate."""
import csv, json, statistics, sys
from collections import defaultdict
from pathlib import Path

root=Path(sys.argv[1])
read=lambda p:list(csv.DictReader(p.open(encoding='utf-8-sig')))
def read_tree(path):
    # VTune prefixes tree indentation BEFORE a quoted CSV field. Preserve it,
    # but remove it for CSV parsing so commas in C++ signatures remain quoted.
    lines=path.read_text(encoding='utf-8-sig').splitlines()
    header=next(csv.reader([lines[0]])); rows=[]
    for line in lines[1:]:
        indent=line[:len(line)-len(line.lstrip(' '))]
        values=next(csv.reader([line.lstrip(' ')]))
        assert len(values)==len(header),(path,line)
        values[0]=indent+values[0]
        rows.append(dict(zip(header,values)))
    return rows
median=statistics.median
def percentile(values,p):
    values=sorted(values)
    return values[min(len(values)-1,int((len(values)-1)*p))]

result={'timing':[],'memory':[],'profiles':[]}
for p in sorted(root.glob('timing-*-phases.csv')):
    rows=read(p);groups=defaultdict(list);cycles=defaultdict(int)
    for r in rows:groups[r['phase']].append(int(r['ns']));cycles[int(r['cycle'])]+=int(r['ns'])
    pair=int(p.name.split('-')[1])
    original=read(root/f'timing-{pair}-original.csv')
    # Original benchmark reports a duration per batch, not per coroutine.resume().
    nsfield=next(k for k in original[0] if k in ('ns','elapsed_ns','duration_ns','nanoseconds'))
    old=[int(r[nsfield]) for r in original]
    item={'pair':pair,'phases':{},'sum_mean_ns':statistics.mean(cycles.values()),
          'original_mean_ns':statistics.mean(old),'sum_p99_ns':percentile(list(cycles.values()),.99)}
    for phase,values in groups.items():
        item['phases'][phase]={'mean_ns':statistics.mean(values),'p50_ns':median(values),
                              'p95_ns':percentile(values,.95),'p99_ns':percentile(values,.99),
                              'ns_per_cycle':statistics.mean(values)/10000}
    item['probe_delta_percent']=100*(item['sum_mean_ns']/item['original_mean_ns']-1)
    result['timing'].append(item)

for p in sorted(root.glob('memory-*.csv')):
    rows=read(p);groups=defaultdict(list)
    for r in rows:groups[r['phase']].append(r)
    item={'file':p.name,'scope':'VM allocator requests only; separate instrumented runs, not timing evidence',
          'phases':{}}
    for phase,values in groups.items():
        fields=['threads_created','threads_released','vm_allocations','vm_reallocations','vm_frees',
                'vm_requested_bytes','vm_released_bytes']
        item['phases'][phase]={k:sum(int(r[k]) for r in values) for k in fields}
        item['phases'][phase]['batches']=len(values)
    result['memory'].append(item)

for repetition in (1,2):
    name=f'roi-{repetition}'
    if not (root/(name+'-top-down.csv')).exists():continue
    modules=read(root/(name+'-modules.csv'))
    total=sum(float(r['CPU Time']) for r in modules)
    tasks=read(root/(name+'-tasks.csv'))
    phases={}
    # Tail calls remove some noinline wrapper frames. Task filters, not wrapper
    # names, define phases. Each filtered report's inclusive total is 100%.
    for phase in ['register','deliver','resume_cleanup']:
        task=next(t for t in tasks if t['Task Type']==phase)
        sampled=float(task['CPU Time']); inclusive=sampled/total*100
        tree=read_tree(root/f'{name}-{phase}-top-down.csv')
        subtree=[{'name':r['Function Stack'].strip(),
                  'inclusive_percent':float(r['CPU Time:Total'] or 0),
                  'self_seconds':float(r['CPU Time:Self'] or 0)} for r in tree]
        by_self=defaultdict(float)
        for n in subtree:by_self[n['name']]+=n['self_seconds']
        gc=sum(v for k,v in by_self.items() if k.startswith(('gc_','lj_gc')))
        allocator=sum(v for k,v in by_self.items() if k in ['lj_alloc_malloc','lj_alloc_free','lj_alloc_realloc'])
        focus=['acquireContinuation','invokePreparedStep','waitEvent','eventSource','reserveAwaitable',
               'beginSuspension','registerWait','claim','completeClaimedEventWaiter','finishAwaitableOwner',
               'resumeOne','takeAwaitable','resumeLuaContinuation','pushResumeValue','destroyContinuation',
               'destroyLuaContinuation','setMethodRunnable','drainExternalCompletions',
               'lua_newthread','lj_gc_step','gc_sweep','gc_propagate_gray','atomic',
               'lj_state_new','luaL_ref','luaL_unref','resumeLuaVm','findExecutionInstance',
               'executionRecord','active','claimEventWaiters','eraseEventWaiter',
               'cancel','detachSource','valid','releaseSource','eraseAwaitableRecord',
               'unlinkAwaitableOwnership','ScriptOwnedResumeValue','ScriptOwnedBytes']
        selected=[]
        for fn in focus:
            found=[n for n in subtree if n['name'].endswith('::'+fn) or n['name']==fn]
            if found:
                share=sum(n['inclusive_percent'] for n in found)
                selected.append({'function':fn,'inclusive_percent_of_phase':share,
                                 'inclusive_percent_of_roi':share*inclusive/100,
                                 'self_seconds':sum(n['self_seconds'] for n in found),
                                 'note':'Inclusive entries overlap; do not add them.'})
        phases[phase]={'whole_roi_percent':inclusive,'sampled_seconds':sampled,
                        'gc_self_seconds':gc,'allocator_self_seconds':allocator,
                        'top_self':[{'function':k,'seconds':v} for k,v in sorted(by_self.items(),key=lambda x:-x[1])[:18]],
                        'inclusive_functions':selected}
    result['profiles'].append({'run':name,'roi_sampled_cpu_seconds':total,'phases':phases})

(root/'analysis.json').write_text(json.dumps(result,indent=2))
for x in result['timing']:
    print('PAIR',x['pair'],'original ms',round(x['original_mean_ns']/1e6,4),
          'phases ms',round(x['sum_mean_ns']/1e6,4),'delta%',round(x['probe_delta_percent'],2))
if result['timing']:
    for phase in result['timing'][0]['phases']:
        print('PHASE',phase,'mean ms',median(x['phases'][phase]['mean_ns'] for x in result['timing'])/1e6,
              'p99 ms',median(x['phases'][phase]['p99_ns'] for x in result['timing'])/1e6)
for x in result['profiles']:
    print('PROFILE',x['run'],x['roi_sampled_cpu_seconds'])
    for k,v in x['phases'].items():print(k,round(v['whole_roi_percent'],2),v['inclusive_functions'])
