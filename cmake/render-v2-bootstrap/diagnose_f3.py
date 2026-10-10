"""Targeted, immutable-binary paired measurements. Never discards a sample.

AA/BB measure process-order/environment variability; AB is the production comparison.
Confidence intervals resample whole process pairs, not correlated within-process samples.
No pass/fail decision is inferred from one invocation of this diagnostic.
"""
import argparse
import ctypes
import hashlib
import json
import random
import statistics
import subprocess
import time
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quantile(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * q))]


def summarize(pairs):
    output = {}
    rng = random.Random(20261010)
    for key in ('p50_us', 'p95_us'):
        changes = [100 * (r['second'][key] / r['first'][key] - 1) for r in pairs]
        bootstrap = [statistics.median(rng.choices(changes, k=len(changes))) for _ in range(10000)]
        pooled = [[x for r in pairs for x in r[side]['samples_us']] for side in ('first', 'second')]
        q = .5 if key == 'p50_us' else .95
        cluster_bootstrap = []
        for _ in range(2000):
            selected = rng.choices(pairs, k=len(pairs))
            values = [[x for r in selected for x in r[side]['samples_us']] for side in ('first', 'second')]
            cluster_bootstrap.append(100 * (quantile(values[1], q) / quantile(values[0], q) - 1))
        output[key] = {
            'paired_percent_changes': changes,
            'paired_median_percent': statistics.median(changes),
            'paired_median_95pct_bootstrap_ci': [quantile(bootstrap, .025), quantile(bootstrap, .975)],
            'paired_distribution_min_q25_q75_max': [min(changes), quantile(changes, .25), quantile(changes, .75), max(changes)],
            'pooled_first_us': quantile(pooled[0], q), 'pooled_second_us': quantile(pooled[1], q),
            'pooled_percent': 100 * (quantile(pooled[1], q) / quantile(pooled[0], q) - 1),
            'pooled_95pct_pair_cluster_ci': [quantile(cluster_bootstrap, .025), quantile(cluster_bootstrap, .975)],
        }
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pairs', type=int, default=40)
    parser.add_argument('--size', type=int, default=256)
    parser.add_argument('--modes', nargs='+', choices=['AA', 'BB', 'AB'], default=['AA', 'BB', 'AB'])
    parser.add_argument('--affinity', type=int, default=4)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    kernel.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    kernel.GetPriorityClass.argtypes = [ctypes.c_void_p]
    kernel.QueryProcessCycleTime.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulonglong)]
    kernel.GetProcessTimes.argtypes = [ctypes.c_void_p, *([ctypes.POINTER(ctypes.c_ulonglong)] * 4)]
    if not kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(), args.affinity):
        raise ctypes.WinError(ctypes.get_last_error())
    binaries = {'A': args.before.resolve(), 'B': args.after.resolve()}
    hashes = {label: sha(path) for label, path in binaries.items()}
    results = {}
    for mode in args.modes:
        rows = []
        for pair in range(args.pairs):
            row = {'pair': pair}
            # Fixed, predeclared alternation AB/BA. AA and BB use the same ordering control.
            phases = [('first', mode[0]), ('second', mode[1])]
            if pair % 2:
                phases.reverse()
            row['order'] = phases
            for side, label in phases:
                command = [str(binaries[label]), str(args.size)]
                start = time.time_ns()
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           creationflags=subprocess.ABOVE_NORMAL_PRIORITY_CLASS)
                handle = ctypes.c_void_p(int(process._handle))
                affinity, system_mask = ctypes.c_size_t(), ctypes.c_size_t()
                if not kernel.GetProcessAffinityMask(handle, ctypes.byref(affinity), ctypes.byref(system_mask)):
                    raise ctypes.WinError(ctypes.get_last_error())
                priority = kernel.GetPriorityClass(handle)
                if affinity.value != args.affinity or priority != subprocess.ABOVE_NORMAL_PRIORITY_CLASS:
                    raise RuntimeError('Child scheduling settings differ from declared policy')
                stdout, stderr = process.communicate()
                cycles = ctypes.c_ulonglong()
                times = [ctypes.c_ulonglong() for _ in range(4)]
                if not kernel.QueryProcessCycleTime(handle, ctypes.byref(cycles)):
                    raise ctypes.WinError(ctypes.get_last_error())
                if not kernel.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
                    raise ctypes.WinError(ctypes.get_last_error())
                elapsed = time.time_ns() - start
                name = f'{mode}-{pair:03d}-{side}'
                (args.output / (name + '.stdout')).write_bytes(stdout)
                (args.output / (name + '.stderr')).write_bytes(stderr)
                receipt = {'command': command, 'exit_code': process.returncode, 'start_unix_ns': start, 'wall_ns': elapsed,
                           'effective_affinity': affinity.value, 'effective_priority': priority,
                           'whole_process_cycles': cycles.value, 'whole_process_kernel_100ns': times[2].value,
                           'whole_process_user_100ns': times[3].value}
                (args.output / (name + '.json')).write_text(json.dumps(receipt, indent=2))
                if process.returncode:
                    raise RuntimeError(receipt)
                row[side] = json.loads(stdout)
                row[side]['process'] = receipt
            for key in ('passes', 'resources', 'subresource_cells', 'dependencies', 'versions', 'hazards', 'repeats', 'allocations', 'bytes'):
                if row['first'].get(key) != row['second'].get(key):
                    raise RuntimeError(f'Workload mismatch: {key}')
            rows.append(row)
        results[mode] = {'pairs': rows, 'summary': summarize(rows)}
        print(json.dumps({mode: results[mode]['summary']}, indent=2), flush=True)
    if hashes != {label: sha(path) for label, path in binaries.items()}:
        raise RuntimeError('Binary changed during measurement')
    report = {'binaries': {label: {'path': str(path), 'sha256': hashes[label]} for label, path in binaries.items()},
              'affinity': args.affinity, 'priority': 'ABOVE_NORMAL', 'pairs': args.pairs,
              'size': args.size, 'script_sha256': sha(Path(__file__)), 'results': results,
              'policy': 'All samples retained; separate processes; fixed alternating order; pair-cluster bootstrap; no automatic waiver'}
    (args.output / 'report.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
