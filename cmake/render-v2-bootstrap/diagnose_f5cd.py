"""One bounded Runtime diagnostic; no production mutation or qualification waiver."""
import argparse
import ctypes
import datetime
import hashlib
import json
import random
import re
import statistics
import subprocess
import time
from pathlib import Path


VERSIONS = {'A': 'e662ff59844cf69f00d94341768df58ddc7f44b0',
            'B': 'e0c31de6678394c37ad7ed1ffc2d77231bccb9cc'}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf-8')


def prepare(source, output):
    """Derive cold metadata only; original timed statements remain byte-identical."""
    output.mkdir(parents=True, exist_ok=False)
    profile = source / 'modules/function/render/runtime/test/ProfileFrame.cpp'
    text = profile.read_text()
    anchor = '            device = backend.device.native();'
    assert text.count(anchor) == 1
    extra = r'''
            for (const auto& variant : backend.variants)
            {
                if (!variant)
                {
                    continue;
                }
                std::size_t cells = 0;
                for (std::uint32_t r = 1; r <= variant->code.logical().identity().resources.size(); ++r)
                {
                    auto count = variant->code.resourceStateCount(GraphResourceId{r});
                    if (!count)
                    {
                        std::terminate();
                    }
                    cells += *count;
                }
                std::printf(
                    "F5CD slots=%zu cells=%zu imports=%zu max_views=%u fif=%u\n",
                    variant->targets.size(), cells, variant->code.logical().imports().size(),
                    backend.capacity.views, backend.capacity.frames_in_flight
                );
                std::printf("F5CD_NATIVE %s\n", variant->invocations.diagnostics().c_str());
            }
'''
    text = text.replace(anchor, anchor + extra)
    include = '#include "../src/Frame.cpp"'
    assert text.count(include) == 1
    text = text.replace(include, '#include "' + (profile.parent.parent / 'src/Frame.cpp').as_posix() + '"')
    generated = output / 'ProfileFrameMetadata.cpp'
    generated.write_text(text, encoding='utf-8')
    hook = output / 'profile.cmake'
    hook.write_text('''function(f5cd_profile_metadata)
    get_target_property(sources render_runtime_profile SOURCES)
    list(FILTER sources EXCLUDE REGEX "/ProfileFrame\\.cpp$")
    list(APPEND sources "''' + generated.as_posix() + '''")
    set_property(TARGET render_runtime_profile PROPERTY SOURCES "${sources}")
endfunction()

cmake_language(DEFER CALL f5cd_profile_metadata)
''', encoding='utf-8')
    write(output / 'transformation.json', {'source': str(profile), 'source_sha256': digest(profile),
          'generated': str(generated), 'generated_sha256': digest(generated), 'hook_sha256': digest(hook),
          'change': 'one cold metadata block, absolute original production include; no timed body changes'})


def quantiles(values):
    ordered = sorted(values)
    assert len(ordered) == 240
    return {'p50_ns': ordered[120], 'p95_ns': ordered[228], 'max_ns': ordered[-1], 'samples_ns': values}


def run(identity, output):
    output.mkdir(parents=True, exist_ok=False)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    assert kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(), 4)
    kernel.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.c_void_p] * 4
    kernel.GetProcessIoCounters.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    frequency = ctypes.c_longlong()
    assert kernel.QueryPerformanceFrequency(ctypes.byref(frequency))
    protocol = Path(__file__).resolve().parents[2] / 'docs/render-v2/F5CD_RUNTIME_DIAGNOSTIC_PROTOCOL.md'
    locked = {'identity': identity, 'script_sha256': digest(__file__), 'protocol_sha256': digest(protocol),
              'binaries': {v: digest(Path(identity[v]['build']) / 'render_runtime_benchmark.exe') for v in VERSIONS},
              'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'qpc_hz': frequency.value,
              'affinity_mask': 4, 'priority': 'ABOVE_NORMAL',
              'power_plan': subprocess.check_output(['powercfg', '/getactivescheme']).decode(errors='replace')}
    for version, expected in VERSIONS.items():
        actual = subprocess.check_output(['git', '-C', identity[version]['source'], 'rev-parse', 'HEAD'], text=True).strip()
        assert actual == expected
        assert not subprocess.check_output(['git', '-C', identity[version]['source'], 'status', '--porcelain'])
    write(output / 'locked-inputs.json', locked)
    schedule = []
    rng = random.Random(20261011)
    for block in range(4):
        cells = [(s, v, mode) for s, v in ((1, 1), (1, 4), (2, 4))
                 for mode in ('AA', 'BB', 'AB') if mode == 'AB' or block < 2]
        rng.shuffle(cells)
        for scenes, views, mode in cells:
            for arm in (('B', 'A') if block % 2 else ('A', 'B')):
                schedule.append({'block': block, 'scenes': scenes, 'views': views, 'mode': mode,
                                 'arm': arm, 'version': mode[0 if arm == 'A' else 1]})
    assert len(schedule) == 48
    write(output / 'schedule.json', schedule)
    rows = []
    for index, cell in enumerate(schedule):
        build = Path(identity[cell['version']]['build'])
        exe = build / 'render_runtime_benchmark.exe'
        assert digest(exe) == locked['binaries'][cell['version']]
        command = [str(exe), str(build / 'render_vulkan/test/shader'), str(cell['scenes']), str(cell['views'])]
        started = time.time_ns()
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=subprocess.ABOVE_NORMAL_PRIORITY_CLASS)
        stdout, stderr = process.communicate()
        times = [ctypes.c_ulonglong() for _ in range(4)]
        times_ok = kernel.GetProcessTimes(int(process._handle), *(ctypes.byref(x) for x in times))
        io = (ctypes.c_ulonglong * 6)()
        io_ok = kernel.GetProcessIoCounters(int(process._handle), ctypes.byref(io))
        class Memory(ctypes.Structure):
            _fields_ = [('size', ctypes.c_ulong), ('page_faults', ctypes.c_ulong)] + [
                (n, ctypes.c_size_t) for n in ('peak_ws', 'ws', 'peak_paged', 'paged', 'peak_nonpaged',
                                              'nonpaged', 'pagefile', 'peak_pagefile')]
        memory = Memory()
        memory.size = ctypes.sizeof(memory)
        psapi = ctypes.WinDLL('psapi', use_last_error=True)
        psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
        memory_ok = psapi.GetProcessMemoryInfo(int(process._handle), ctypes.byref(memory), memory.size)
        record = {**cell, 'command': command, 'exit_code': process.returncode, 'pid': process.pid,
                  'exe_sha256': digest(exe), 'start_unix_ns': started, 'end_unix_ns': time.time_ns(),
                  'cpu_100ns': [times[2].value, times[3].value] if times_ok else None,
                  'io': list(io) if io_ok else None,
                  'memory': {n: getattr(memory, n) for n, _ in Memory._fields_} if memory_ok else None,
                  'memory_error': 0 if memory_ok else ctypes.get_last_error()}
        name = f'{index:02d}-{cell["mode"]}-{cell["arm"]}-{cell["scenes"]}-{cell["views"]}'
        (output / (name + '.stdout.log')).write_bytes(stdout)
        (output / (name + '.stderr.log')).write_bytes(stderr)
        write(output / (name + '.command.json'), record)
        assert process.returncode == 0, name
        lines = stdout.decode().splitlines()
        items = [json.loads(line) for line in lines if line.startswith('{')]
        assert len(items) == 6 and all(len(item['samples_ns']) == 240 for item in items), name
        runtime = next(line for line in lines if line.startswith('RUNTIME '))
        assert 'allocations=0 bytes=0' in runtime and 'compiles=1' in runtime, name
        metadata = [line for line in lines if line.startswith('F5CD ')]
        assert len(metadata) == 1
        native = json.loads(next(line[len('F5CD_NATIVE '):] for line in lines if line.startswith('F5CD_NATIVE ')))
        row = {**record, 'metrics': items, 'runtime': runtime, 'metadata': metadata, 'native': native,
               'oracle': 'PASS: unchanged benchmark checks every readback value before successful exit'}
        total = next(x for x in items if x['case'].startswith('runtime_CPU_frame'))
        nested = next(x for x in items if x['case'].startswith('native_import_validation'))
        remaining = [a - b for a, b in zip(total['samples_ns'], nested['samples_ns'])]
        assert min(remaining) >= 0, 'timer nesting violated'
        items.append({'case': 'remaining_CPU_frame_after_nested_preparation', **quantiles(remaining)})
        rows.append(row)
        with (output / 'rows.jsonl').open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(row) + '\n')
        print(f'completed {index + 1}/48 {name}', flush=True)
    analyze(output)


def analyze(output):
    rows = [json.loads(line) for line in (output / 'rows.jsonl').read_text().splitlines()]
    assert len(rows) == 48
    result = []
    for scenes, views in ((1, 1), (1, 4), (2, 4)):
        for case in (x['case'] for x in rows[0]['metrics']):
            for mode in ('AA', 'BB', 'AB'):
                group = [r for r in rows if r['scenes'] == scenes and r['views'] == views and r['mode'] == mode]
                for key in ('p50_ns', 'p95_ns', 'max_ns'):
                    pairs = []
                    for block in range(4 if mode == 'AB' else 2):
                        values = {r['arm']: next(x[key] for x in r['metrics'] if x['case'] == case)
                                  for r in group if r['block'] == block}
                        pairs.append({'block': block, **values, 'difference_ns': values['B'] - values['A'],
                                      'percent': 100 * (values['B'] / values['A'] - 1) if values['A'] else None})
                    result.append({'scenes': scenes, 'views': views, 'case': case, 'mode': mode, 'metric': key,
                                   'pairs': pairs, 'median_percent': statistics.median(p['percent'] for p in pairs),
                                   'A_median_ns': statistics.median(p['A'] for p in pairs),
                                   'B_median_ns': statistics.median(p['B'] for p in pairs)})
    write(output / 'analysis.json', {'processes': 48, 'no_exclusions': True, 'formal_gate': 'UNCHANGED PARTIAL',
                                    'small_sample_qualification': 'NOT_APPLICABLE', 'pairs': result})


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['prepare', 'run', 'analyze'])
    parser.add_argument('--source', type=Path)
    parser.add_argument('--identity', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.action == 'prepare':
        prepare(args.source.resolve(), args.output.resolve())
    elif args.action == 'run':
        run(json.loads(args.identity.read_text()), args.output)
    else:
        analyze(args.output)
