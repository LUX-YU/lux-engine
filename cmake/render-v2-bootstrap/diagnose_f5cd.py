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



def prepare_local(source, output):
    """One separate diagnostic overlay; never compiled into production targets."""
    prepare(source, output)
    native = source / 'modules/function/render/vulkan/src/graph/Frame.cpp'
    original = native.read_text()
    anchor = '    static RenderResult<void> validateImports('
    assert original.count(anchor) == 1
    original = original.replace(anchor, '    static RenderResult<void> validateImportsObservedBody(')
    prefix = r'''#include <chrono>
#include <cstdio>
#include <array>
#define prepareSlot prepareSlotUninstrumented
namespace
{
    struct F5cdObservation
    {
        double ns;
        std::size_t slots, cells, tickets, pending, probes;
    };
    std::array<F5cdObservation, 8192> f5cd_observations{};
    unsigned f5cd_count{};
}
'''
    wrapper = r'''
    static RenderResult<void> validateImports(
        const NativeGraphBacking& plan,
        const FrameGraphBindings& bindings,
        std::span<const NativeImportBinding> imports
    ) noexcept
    {
        F5cdObservation value{};
        value.slots = plan.slots.size();
        bool found_pending = false;
        for (const auto& slot : plan.slots)
        {
            for (const auto& ticket : slot.cell_tickets)
            {
                ++value.cells;
                if (!found_pending)
                {
                    ++value.probes;
                }
                if (ticket)
                {
                    ++value.tickets;
                    if (ticket->serial() > ticket->owner().completed())
                    {
                        ++value.pending;
                        found_pending = true;
                    }
                }
            }
        }
        const auto start = std::chrono::steady_clock::now();
        auto result = validateImportsObservedBody(plan, bindings, imports);
        value.ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        if (f5cd_count >= f5cd_observations.size())
        {
            std::terminate();
        }
        f5cd_observations[f5cd_count++] = value;
        return result;
    }

'''
    anchor = '    RenderResult<void> prepareSlot('
    assert original.count(anchor) == 1
    original = original.replace(anchor, wrapper + anchor)
    tail = r'''
#undef prepareSlot
void recordRuntimeImportProfile(double nanoseconds) noexcept;
namespace lux::render::vulkan::detail
{
    RenderResult<void> prepareSlot(
        NativeGraphBacking& plan,
        GraphSlot& slot,
        const FrameGraphBindings& bindings,
        std::span<const NativeImportBinding> imports,
        const GraphSlot* scene_source
    ) noexcept
    {
        const auto start = std::chrono::steady_clock::now();
        auto result = prepareSlotUninstrumented(plan, slot, bindings, imports, scene_source);
        recordRuntimeImportProfile(
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count()
        );
        return result;
    }
}
void finishF5cdNativeProfile()
{
    std::printf("F5CD_DETAIL {\"columns\":[\"ns\",\"slots\",\"cells\",\"tickets\",\"pending\",\"probes\"],\"rows\":[");
    for (unsigned i = 0; i < f5cd_count; ++i)
    {
        const auto& v = f5cd_observations[i];
        std::printf("%s[%.0f,%zu,%zu,%zu,%zu,%zu]", i ? "," : "", v.ns, v.slots, v.cells, v.tickets, v.pending, v.probes);
    }
    std::puts("]}");
}
'''
    generated = output / 'ProfileNativeObserved.cpp'
    generated.write_text(prefix + original + tail, encoding='utf-8')
    frame = output / 'ProfileFrameMetadata.cpp'
    text = frame.read_text()
    text = text.replace('void finishRuntimeProfile()\n{', 'void finishF5cdNativeProfile();\nvoid finishRuntimeProfile()\n{\n    finishF5cdNativeProfile();')
    text = text.replace('    unsigned completed{};', '    unsigned completed{};\n    std::array<std::array<std::uint64_t, 3>, 264> f5cd_submissions{};')
    anchor = '        const auto start = Clock::now();'
    assert text.count(anchor) == 1
    text = text.replace(anchor, '''        std::array<std::uint64_t, 3> before{};
        for (unsigned q = 0; q < 3; ++q)
        {
            before[q] = backend.queues[q]->submitted();
        }
''' + anchor)
    anchor = '        if (!result)\n'
    assert text.count(anchor) == 1
    text = text.replace(anchor, '''        if (serial <= 264)
        {
            for (unsigned q = 0; q < 3; ++q)
            {
                f5cd_submissions[serial - 1][q] = backend.queues[q]->submitted() - before[q];
            }
        }
''' + anchor)
    text = text.replace('    finishF5cdNativeProfile();', '''    finishF5cdNativeProfile();
    std::printf("F5CD_SUBMISSIONS [");
    for (unsigned i = 0; i < 264; ++i)
    {
        const auto& v = f5cd_submissions[i];
        std::printf("%s[%llu,%llu,%llu]", i ? "," : "", v[0], v[1], v[2]);
    }
    std::puts("]");''')
    frame.write_text(text, encoding='utf-8')
    benchmark = source / 'modules/function/render/runtime/test/RuntimeBenchmark.cpp'
    text = benchmark.read_text().replace('CHECK(argc == 4);', 'CHECK(argc == 5);')
    anchor = '        RuntimeNativeOptions native;'
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + '\n        native.capacity.frames_in_flight = static_cast<unsigned>(std::atoi(argv[4]));\n        CHECK(native.capacity.frames_in_flight == 2 || native.capacity.frames_in_flight == 3);')
    (output / 'RuntimeBenchmarkObserved.cpp').write_text(text, encoding='utf-8')
    hook = output / 'profile.cmake'
    text = hook.read_text().replace('endfunction()', '''    get_target_property(native_sources render_vulkan_graph_profile SOURCES)
    list(FILTER native_sources EXCLUDE REGEX "/ProfileNativeFrame\\\\.cpp$")
    list(APPEND native_sources "''' + generated.as_posix() + '''")
    set_property(TARGET render_vulkan_graph_profile PROPERTY SOURCES "${native_sources}")
    get_target_property(bench_sources render_runtime_benchmark SOURCES)
    list(FILTER bench_sources EXCLUDE REGEX "/RuntimeBenchmark\\\\.cpp$")
    list(APPEND bench_sources "''' + (output / 'RuntimeBenchmarkObserved.cpp').as_posix() + '''")
    set_property(TARGET render_runtime_benchmark PROPERTY SOURCES "${bench_sources}")
endfunction()''')
    hook.write_text(text, encoding='utf-8')
    write(output / 'local-transformation.json', {'native_source_sha256': digest(native),
          'purpose': 'separate bounded local observation; original predicates/return paths retained',
          'files': {p.name: digest(p) for p in output.iterdir() if p.is_file()}})


def run_local(identity, output):
    output.mkdir(parents=True, exist_ok=False)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    assert kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(), 4)
    schedule = [(v, w, f) for w in (1, 4) for f in (2, 3) for v in ('A', 'B')]
    random.Random(20261011).shuffle(schedule)
    write(output / 'lock.json', {'schedule': schedule, 'script_sha256': digest(__file__),
          'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'binaries': {v: digest(Path(identity[v]['build']) / 'render_runtime_benchmark.exe') for v in VERSIONS}})
    for index, (version, views, fif) in enumerate(schedule):
        build = Path(identity[version]['build'])
        command = [str(build / 'render_runtime_benchmark.exe'), str(build / 'render_vulkan/test/shader'), '1', str(views), str(fif)]
        p = subprocess.run(command, capture_output=True, creationflags=subprocess.ABOVE_NORMAL_PRIORITY_CLASS)
        stem = f'{index}-{version}-{views}-{fif}'
        (output / (stem + '.stdout.log')).write_bytes(p.stdout)
        (output / (stem + '.stderr.log')).write_bytes(p.stderr)
        write(output / (stem + '.command.json'), {'command': command, 'exit_code': p.returncode})
        assert p.returncode == 0, stem
        assert b'allocations=0 bytes=0' in p.stdout
        assert b'F5CD_DETAIL ' in p.stdout and b'F5CD_SUBMISSIONS ' in p.stdout
        print('local ' + stem + ' PASS', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['prepare', 'run', 'analyze', 'prepare-local', 'run-local'])
    parser.add_argument('--source', type=Path)
    parser.add_argument('--identity', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.action == 'prepare':
        prepare(args.source.resolve(), args.output.resolve())
    elif args.action == 'prepare-local':
        prepare_local(args.source.resolve(), args.output.resolve())
    elif args.action == 'run-local':
        run_local(json.loads(args.identity.read_text()), args.output)
    elif args.action == 'run':
        run(json.loads(args.identity.read_text()), args.output)
    else:
        analyze(args.output)
