"""Summarize all recorded trials without selecting a faster subset or relabelling raw identities."""
import json, statistics, sys
from pathlib import Path
root=Path(sys.argv[1]).resolve()
all_rows=[]
for directory in sorted((root/'costs').iterdir()):
    path=directory/'runs.json'
    if not path.exists(): continue
    runs=json.loads(path.read_text())
    families=sorted({r.get('family','physics') for r in runs})
    for family in families:
        selected=[r for r in runs if r.get('family','physics')==family]
        pairs=[]
        delta_ns=[]
        tail_pairs=[]
        for pair in sorted({r['pair'] for r in selected}):
            sides={r['side']:r for r in selected if r['pair']==pair}
            if len(sides)!=2 or not all(r['valid'] for r in sides.values()): continue
            a,b=sides['a'],sides['b']
            if a['final_counters']!=b['final_counters']: raise RuntimeError('Unequal business '+str(path))
            pairs.append(100*(b['total_ns']/a['total_ns']-1))
            assert a['operations']==b['operations']
            delta_ns.append((b['total_ns']-a['total_ns'])/a['operations'])
            tail_pairs.append(100*(b['p99_batch_ns']/a['p99_batch_ns']-1))
        complete=len(pairs)==5 and len(selected)==10 and all(r['valid'] for r in selected)
        sides={side:[r for r in selected if r['side']==side and r['valid']] for side in ['a','b']}
        med=lambda side,key:statistics.median(r[key] for r in sides[side]) if sides[side] else None
        all_rows.append(dict(comparison=directory.name,family=family,valid_pairs=len(pairs),complete=complete,
            operation_unit=selected[0]['operation_unit'],operations=selected[0]['operations'],
            parent_median_total_ns=med('a','total_ns'),candidate_median_total_ns=med('b','total_ns'),
            parent_median_ns_per_operation=med('a','ns_per_frame' if family=='physics' else 'ns_per_operation'),
            candidate_median_ns_per_operation=med('b','ns_per_frame' if family=='physics' else 'ns_per_operation'),
            paired_deltas_percent=pairs,paired_median_percent=statistics.median(pairs) if pairs else None,
            paired_delta_ns_per_operation=delta_ns,
            paired_median_delta_ns_per_operation=statistics.median(delta_ns) if delta_ns else None,
            paired_p99_deltas_percent=tail_pairs,
            paired_median_p99_delta_percent=statistics.median(tail_pairs) if tail_pairs else None,
            faster_pairs=sum(p<0 for p in pairs),parent_median_p50_ns=med('a','p50_batch_ns'),
            candidate_median_p50_ns=med('b','p50_batch_ns'),parent_median_p95_ns=med('a','p95_batch_ns'),
            candidate_median_p95_ns=med('b','p95_batch_ns'),parent_median_p99_ns=med('a','p99_batch_ns'),
            candidate_median_p99_ns=med('b','p99_batch_ns'),final_counters=selected[-1]['final_counters'],
            error_scope=selected[-1]['error_scope'],errors_observed=selected[-1]['errors_observed'],
            resume_wall_latency_ns=None,cost_qualification=False))
(root/'cost-summary.json').write_text(json.dumps(all_rows,indent=2))
lines=['| 比较 | 工作负载 | ns/实际操作 A → B | 总 ROI ms A → B | 配对变化中位 | 更快对数 |',
    '|---|---|---:|---:|---:|---:|']
for row in all_rows:
    if not row['complete']:
        lines.append(f"| {row['comparison']} | {row['family']} | 未完成 | — | — | {row['valid_pairs']}/5 有效 |")
        continue
    lines.append('| {} | {} | {:.2f} → {:.2f} | {:.2f} → {:.2f} | {:+.2f}% | {}/5 |'.format(
        row['comparison'],row['family'],row['parent_median_ns_per_operation'],
        row['candidate_median_ns_per_operation'],row['parent_median_total_ns']/1e6,
        row['candidate_median_total_ns']/1e6,row['paired_median_percent'],row['faster_pairs']))
(root/'cost-summary.md').write_text('\n'.join(lines)+'\n')
print(len(all_rows),'case families;',sum(r['complete'] for r in all_rows),'complete')
