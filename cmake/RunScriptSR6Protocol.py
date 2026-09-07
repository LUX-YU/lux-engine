"""Narrow installed-runtime diagnostic using the existing Event fixture and real Simulation clock.

Separate from the public SDK value consumer and all timing runs. No production source is patched.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from RunScriptSR2Probes import run, truncate_main

MAIN = r'''
int main()
{
    HarnessOptions options;
    options.real_clock = true;
    options.occurrence_capacity = 32U;
    options.limits = {32U, 1U, 32U, 32U, 32U, 32U, 64U, 3U, 32U, 32U, 32U, 32U};
    Harness harness{options};
    observed_clock = &harness.clock_owner->clock();
    for (unsigned i{}; i < 17U; ++i) harness.recordBroadcastStart(1);
    assert(deliverEndpoint(harness.broadcast_start_bridge) == 17U);
    assert(harness.backend_state.step_calls == 17U);
    harness.recordBroadcastWait(73);
    assert(deliverEndpoint(harness.broadcast_wait_bridge) == 1U);
    harness.broadcast_wait.reset();
    const auto ready = harness.system->stats();
    assert(observed_clock->snapshot().step_index == 0U);
    assert(ready.active_event_waiters == 0U && ready.resume_queue_depth == 17U);
    assert(harness.backend_state.resumes == 0U);
    std::printf("READY step=0 continuations=%zu awaitables=%zu waiters=%zu queue=%zu backing=%zu slots=%zu\n",
        ready.active_continuations, ready.active_awaitables, ready.active_event_waiters,
        ready.resume_queue_depth, ready.awaitable_storage_bytes, ready.awaitable_reserved_slots);
    // Production has stopped; the one delivered occurrence completed every source. No new Hook or Event is injected.
    for (std::uint64_t step{1}; step <= 6U; ++step)
    {
        harness.clock_owner->advance(SimulationDuration{1});
        assert(observed_clock->snapshot().step_index == step);
        assert(harness.system->executeStablePoint());
        const auto expected = std::min<std::size_t>(3U * step, 17U);
        assert(harness.backend_state.resumes == expected);
        assert(harness.system->stats().resume_queue_depth == 17U - expected);
        std::printf("DRAIN step=%llu resumes=%zu queue=%zu\n", step, expected, 17U - expected);
    }
    assert(harness.backend_state.resume_values == std::vector<std::int32_t>(17U, 73));
    assert(harness.backend_state.continuation_destroys == 17U);
    const auto drained = harness.system->stats();
    assert(drained.active_continuations == 0U && drained.active_awaitables == 0U);
    assert(drained.active_event_waiters == 0U && drained.resume_queue_depth == 0U);
    assert(harness.system->failures().empty());
    std::puts("DRAIN_COMPLETE calls=17 resumes=17 destroys=17 errors=0 backlog=0 before_shutdown=1 PASS");
    assert(harness.system->shutdown());
}
'''


def main():
    parser = argparse.ArgumentParser()
    for key in ('source', 'prefix', 'dependencies', 'output'):
        parser.add_argument('--' + key, required=True)
    args = parser.parse_args()
    source_root, root = Path(args.source), Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    fixture = source_root / 'engine/domain/simulation/builtin/script/test/script_system_event_wait_test.cpp'
    text = truncate_main(fixture.read_text(encoding='utf-8-sig'), 'int main(int argc')
    replacements = {
        '#include "../../../scripting/core/test/ScriptEndpointTestAccess.hpp"': '#include "ScriptEndpointTestAccess.hpp"',
        '    struct BackendState final': '    const SimulationClock* observed_clock{};\n    struct BackendState final',
        '        state.resume_values.push_back(value);':
            '        std::printf("RESUME ordinal=%zu step=%llu value=%d\\n", state.resumes,\n'
            '            static_cast<unsigned long long>(observed_clock->snapshot().step_index), value);\n'
            '        state.resume_values.push_back(value);'
    }
    hits = {}
    for old, new in replacements.items():
        hits[old] = text.count(old)
        if hits[old] != 1:
            raise RuntimeError('Expected exactly one diagnostic insertion: ' + old)
        text = text.replace(old, new)
    text = '#define LUX_SCRIPT_SOURCE_PROTOCOL_CLOCK 1\n#include <algorithm>\n' + text + MAIN
    (root / 'protocol.cpp').write_text(text)
    for relative in ('engine/domain/simulation/builtin/script/test/ScriptTestClock.hpp',
                     'engine/domain/simulation/scripting/core/test/ScriptEndpointTestAccess.hpp'):
        p = source_root / relative
        (root / p.name).write_bytes(p.read_bytes())
    (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.22)
project(sr6_protocol LANGUAGES CXX)
find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
add_executable(sr6_protocol protocol.cpp)
target_compile_features(sr6_protocol PRIVATE cxx_std_20)
target_compile_options(sr6_protocol PRIVATE /UNDEBUG)
target_link_libraries(sr6_protocol PRIVATE lux::engine::simulation::simulation_script lux::engine::simulation::composition)
''')
    run(['cmake', '-S', str(root), '-B', str(root / 'build'), '-G', 'Ninja',
         '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
         '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
         '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
         '-DCMAKE_PREFIX_PATH=' + args.prefix + ';' + args.dependencies], root / 'configure.log')
    command = ['cmake', '--build', str(root / 'build'), '--target', 'all', '-j', '4', '--', '-k', '0']
    run(command, root / 'all.log')
    run(command, root / 'noop.log')
    if 'no work to do' not in (root / 'noop.log').read_text():
        raise RuntimeError('Second build is not a no-op')
    env = dict(os.environ)
    clean = [p for p in env['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]
    env['PATH'] = ';'.join([str(Path(p) / 'bin') for p in (args.prefix + ';' + args.dependencies).split(';')] +
                         ['D:/Development/vcpkg/installed/x64-windows/bin'] + clean)
    exe = root / 'build/sr6_protocol.exe'
    run([str(exe)], root / 'protocol.log', env)
    log = (root / 'protocol.log').read_text()
    resumes = [tuple(map(int, values)) for values in re.findall(r'RESUME ordinal=(\d+) step=(\d+) value=(\d+)', log)]
    assert resumes == [(i, 1 + i // 3, 73) for i in range(17)]
    assert log.count('DRAIN_COMPLETE calls=17 resumes=17 destroys=17 errors=0 backlog=0 before_shutdown=1 PASS') == 1
    result = {'source': subprocess.check_output(['git', '-C', str(source_root), 'rev-parse', 'HEAD'], text=True).strip(),
              'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(), 'insertion_hits': hits,
              'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'ready_step': 0,
              'resume_step_delays': [v[1] for v in resumes], 'budget': 3, 'business_completed': 17,
              'drain_steps': 6, 'shutdown_cancellation_counted_as_completion': False}
    (root / 'protocol.json').write_text(json.dumps(result, indent=2))
    print('17 READY results resumed at their expected real steps; drained before shutdown')


if __name__ == '__main__':
    main()
