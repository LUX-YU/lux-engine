"""Paired F2-FIX/F3 CPU regression measurements; run without concurrent builds/GPU tests."""
import argparse
import ctypes
import hashlib
import json
import statistics
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--before', type=Path, required=True)
p.add_argument('--after', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.GetCurrentProcess.restype = ctypes.c_void_p
k.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
assert k.SetProcessAffinityMask(k.GetCurrentProcess(), 4)


def run(exe, args, name):
    command = [str(exe), *args]
    # Equal above-normal scheduling reduces desktop preemption; never realtime priority.
    r = subprocess.run(command, capture_output=True, creationflags=subprocess.ABOVE_NORMAL_PRIORITY_CLASS)
    (a.output / (name + '.log')).write_bytes(r.stdout + r.stderr)
    (a.output / (name + '.command.json')).write_text(json.dumps({'command': command, 'exit_code': r.returncode}))
    assert r.returncode == 0, name
    return json.loads(r.stdout)


rows = []
for pair in range(7):
    phases = [('before', a.before), ('after', a.after)]
    if pair % 2:
        phases.reverse()
    for phase, build in phases:
        data = run(build / 'render_graph/test/render_graph_benchmark.exe', [], f'binding-{pair}-{phase}')
        assert data['allocations'] == data['allocation_bytes'] == 0
        rows.append({'pair': pair, 'phase': phase, **data})
assert len({(r['operations'], r['batch_size'], r['checksum']) for r in rows}) == 1
metrics = {}
for key in ['p50_ns_per_op', 'p95_ns_per_op', 'max_ns_per_op']:
    values = {phase: statistics.median(r[key] for r in rows if r['phase'] == phase) for phase in ['before', 'after']}
    values['percent_change'] = 100 * (values['after'] / values['before'] - 1)
    metrics[key] = values

scale = []
scale_metrics = {}
for n in [64, 128, 256, 512]:
    # Old scale binaries have only 5 or 20 samples/run (their reported p95 is the maximum).
    # Pool all samples from 35 alternating pairs; retain every per-run statistic as well.
    for pair in range(35):
        phases = [('before', a.before), ('after', a.after)]
        if pair % 2:
            phases.reverse()
        for phase, build in phases:
            data = run(build / 'render_graph/test/render_graph_scale_benchmark.exe', [str(n)], f'scale-{n}-{pair}-{phase}')
            scale.append({'pair': pair, 'phase': phase, **data})
    current = [r for r in scale if r['passes'] == n]
    for key in ['passes', 'resources', 'subresource_cells', 'dependencies', 'versions', 'hazards', 'repeats']:
        assert len({r[key] for r in current}) == 1, (n, key)
    scale_metrics[n] = {}
    for key in ['p50_us', 'p95_us', 'max_us']:
        pooled = {phase: sorted(x for r in current if r['phase'] == phase for x in r['samples_us']) for phase in ['before', 'after']}
        quantile = {'p50_us': 0.5, 'p95_us': 0.95, 'max_us': 1}[key]
        values = {phase: data[min(len(data) - 1, int(len(data) * quantile))] for phase, data in pooled.items()}
        values['per_run_median_before'] = statistics.median(r[key] for r in current if r['phase'] == 'before')
        values['per_run_median_after'] = statistics.median(r[key] for r in current if r['phase'] == 'after')
        values['percent_change'] = 100 * (values['after'] / values['before'] - 1)
        scale_metrics[n][key] = values

regressions = {k: v['percent_change'] for k, v in metrics.items() if k != 'max_ns_per_op' and v['percent_change'] > 5}
for n, values in scale_metrics.items():
    for key, value in values.items():
        if key != 'max_us' and value['percent_change'] > 5:
            regressions[f'scale-{n}-{key}'] = value['percent_change']
report = {
    'status': 'PASS' if not regressions else 'REVIEW_REQUIRED', 'regressions': regressions,
    'affinity_mask': 4, 'pairs': 7, 'priority': 'ABOVE_NORMAL',
    'policy': 'binding: seven alternating pairs; scale: 35 alternating pairs, pooled raw samples; no discarded samples',
    'binding': rows, 'binding_metrics': metrics, 'scale': scale, 'scale_metrics': scale_metrics,
    'binaries': {str(f): hashlib.sha256(f.read_bytes()).hexdigest()
                 for b in (a.before, a.after) for f in [b / 'render_graph/test/render_graph_benchmark.exe', b / 'render_graph/test/render_graph_scale_benchmark.exe']}
}
(a.output / 'performance.json').write_text(json.dumps(report, indent=2))
print(json.dumps({'status': report['status'], 'binding': metrics, 'scale': scale_metrics, 'regressions': regressions}, indent=2))
raise SystemExit(0 if not regressions else 1)
