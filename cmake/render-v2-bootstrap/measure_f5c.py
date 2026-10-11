"""Bounded, preregistered F5-C experiment; all children and samples are retained."""
import argparse
import ctypes
import datetime
import hashlib
import json
import random
import statistics
import subprocess
import time
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def interval(values, rng):
    bootstrap = sorted(statistics.median(rng.choices(values, k=len(values))) for _ in range(20000))
    return [bootstrap[500], bootstrap[19500]]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--identity', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    identity = json.loads(args.identity.read_text())
    out = args.output
    out.mkdir(parents=True, exist_ok=False)  # Never replace an earlier experiment.
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    assert kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(), 4)
    kernel.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.c_void_p] * 4
    kernel.GetProcessIoCounters.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    frequency = ctypes.c_longlong()
    assert kernel.QueryPerformanceFrequency(ctypes.byref(frequency))
    protocol = Path(__file__).resolve().parents[2] / 'docs/render-v2/F5C_PERFORMANCE_PROTOCOL.md'
    env = {'qpc_hz': frequency.value, 'affinity_mask': 4, 'priority': 'ABOVE_NORMAL',
           'protocol_sha256': digest(protocol), 'script_sha256': digest(Path(__file__)),
           'power_plan': subprocess.check_output(['powercfg', '/getactivescheme']).decode(errors='replace'),
           'start_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
    (out / 'environment.json').write_text(json.dumps(env, indent=2))
    old = {key: Path('D:/LuxQualification/render-v2-f4cp-92b126f67873/build')
           for key in ('graph', 'shader')}
    new = Path(identity['build'])
    rows = []
    schedule = []
    rng = random.Random(20261016)
    for block in range(41):
        cells = [(w, m) for w in ('graph', 'shader') for m in ('AA', 'BB', 'AB')
                 if m == 'AB' or block < 21]
        rng.shuffle(cells)
        schedule.extend((block, w, m) for w, m in cells)
    (out / 'schedule.json').write_text(json.dumps(schedule, indent=2))
    for block, workload, mode in schedule:
        for arm in (('B', 'A') if block % 2 else ('A', 'B')):
            version = mode[0 if arm == 'A' else 1]
            build = old[workload] if version == 'A' else new
            source = build.parent / 'source'
            exe = build / ('render_vulkan/test/render_vulkan_' +
                           ('graph_benchmark.exe' if workload == 'graph' else 'shader_benchmark.exe'))
            name = f'{block:02d}-{workload}-{mode}-{arm}'
            command = [str(exe), str(build / 'render_vulkan/test/shader')]
            if workload == 'shader':
                command += [str(source / 'modules/function/render/vulkan/test/shader'),
                            str(out / (name + '-evidence'))]
            started = time.time_ns()
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       creationflags=subprocess.ABOVE_NORMAL_PRIORITY_CLASS)
            stdout, stderr = process.communicate()
            times = [ctypes.c_ulonglong() for _ in range(4)]
            times_ok = kernel.GetProcessTimes(int(process._handle), *(ctypes.byref(x) for x in times))
            io = (ctypes.c_ulonglong * 6)()
            io_ok = kernel.GetProcessIoCounters(int(process._handle), ctypes.byref(io))
            # Peak working set/page faults may be unavailable after exit: preserve the OS error, not a zero claim.
            class Memory(ctypes.Structure):
                _fields_ = [('size', ctypes.c_ulong), ('page_faults', ctypes.c_ulong)] + [
                    (n, ctypes.c_size_t) for n in ('peak_ws', 'ws', 'peak_paged', 'paged', 'peak_nonpaged',
                                                  'nonpaged', 'pagefile', 'peak_pagefile')]
            memory = Memory()
            memory.size = ctypes.sizeof(memory)
            psapi = ctypes.WinDLL('psapi', use_last_error=True)
            psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
            memory_ok = psapi.GetProcessMemoryInfo(int(process._handle), ctypes.byref(memory), memory.size)
            record = {'command': command, 'exit_code': process.returncode, 'pid': process.pid,
                      'start_unix_ns': started, 'end_unix_ns': time.time_ns(), 'exe_sha256': digest(exe),
                      'cpu_100ns': {'kernel': times[2].value, 'user': times[3].value} if times_ok else None,
                      'io_counters': list(io) if io_ok else None,
                      'memory': {n: getattr(memory, n) for n, _ in Memory._fields_} if memory_ok else None,
                      'memory_error': 0 if memory_ok else ctypes.get_last_error()}
            (out / (name + '.log')).write_bytes(stdout + stderr)
            (out / (name + '.command.json')).write_text(json.dumps(record, indent=2))
            assert process.returncode == 0, name
            parsed = []
            for line in stdout.decode().splitlines():
                if not line.startswith('{'):
                    continue
                item = json.loads(line)
                if item.get('bounded') or item['case'] in (
                        'fixed_native_binding_recipe', 'native_graph_record_submit_256x256'):
                    assert item['allocations'] == item['allocation_bytes'] == 0, (name, item)
                assert item.get('samples_ns'), (name, 'missing raw timings')
                parsed.append({'block': block, 'workload': workload, 'mode': mode, 'arm': arm, **item})
            assert parsed, name
            rows.extend(parsed)
            with (out / 'rows.jsonl').open('a') as stream:
                for item in parsed:
                    stream.write(json.dumps(item) + '\n')
        print(f'completed block={block + 1}/41 {workload} {mode}', flush=True)
    metrics = {}
    rng = random.Random(20261016)
    for case in sorted({r['case'] for r in rows}):
        metrics[case] = {}
        for mode in ('AA', 'BB', 'AB'):
            current = [r for r in rows if r['case'] == case and r['mode'] == mode]
            result = {}
            for key in ('p50_ns', 'p95_ns', 'max_ns'):
                pairs = []
                for block in range(41 if mode == 'AB' else 21):
                    values = {r['arm']: r[key] for r in current if r['block'] == block}
                    pairs.append({'block': block, **values, 'percent': 100 * (values['B'] / values['A'] - 1)})
                differences = [v['percent'] for v in pairs]
                result[key] = {'pairs': pairs, 'median_percent': statistics.median(differences),
                               'ci95': interval(differences, rng)}
            result['distribution'] = [{
                'block': r['block'], 'arm': r['arm'], 'count': len(r['samples_ns']),
                'min_ns': min(r['samples_ns']), 'max_ns': max(r['samples_ns']),
                'iqr_ns': statistics.quantiles(r['samples_ns'], n=4)[2] -
                          statistics.quantiles(r['samples_ns'], n=4)[0],
            } for r in current]
            metrics[case][mode] = result
    gates = {}
    for case in ('C9_identity_and_candidate', 'complete_native_candidate_including_cook_io',
                 'native_graph_compile_prebuilt_programs', 'fixed_native_binding_recipe',
                 'native_graph_record_submit_256x256'):
        failures = []
        for key in ('p50_ns', 'p95_ns'):
            v = metrics[case]['AB'][key]
            if v['median_percent'] > 5 or v['ci95'][1] > 5:
                failures.append(key + ': AB not bounded within +5%')
            for mode in ('AA', 'BB'):
                v = metrics[case][mode][key]
                if abs(v['median_percent']) > 5 or not v['ci95'][0] <= 0 <= v['ci95'][1]:
                    failures.append(key + ': ' + mode + ' control unresolved')
        gates[case] = {'status': 'PARTIAL' if failures else 'PASS', 'reasons': failures}
    result = {'implementation': identity['implementation'], 'pairs_per_cell': {'AA': 21, 'BB': 21, 'AB': 41}, 'processes': 332,
              'status': 'PASS' if all(gates[k]['status'] == 'PASS' for k in ('fixed_native_binding_recipe', 'native_graph_record_submit_256x256')) else 'PARTIAL',
              'cold_debt': 'PARTIAL; user deferred F4 cold uncertainty only',
              'gates': gates, 'metrics': metrics, 'no_exclusions': True}
    (out / 'comparison.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({'status': result['status'], 'gates': gates}, indent=2))


if __name__ == '__main__':
    main()
