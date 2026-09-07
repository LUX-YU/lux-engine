"""Installed SR-2/SR-3 scale replay; run from a VS development shell, no concurrent builds/tests."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess

from RunScriptSR2Probes import INSTRUMENTATION_HITS, instrument, run, truncate_main


MAIN = r'''
int main(int argc, char** argv)
{
    assert(argc == 3);
    const auto count = static_cast<std::size_t>(std::stoul(argv[1]));
    const bool diagnostics = std::string_view{argv[2]} == "diagnostics";
    assert(count == 8U || count == 8192U);
    Harness harness{count, true, true, true};
    std::vector<HookPointSpec> specifications;
    std::vector<std::string> names;
    names.reserve(count);
    std::vector<std::unique_ptr<HookPoint<void()>>> hooks;
    std::vector<std::unique_ptr<ScriptHookEndpoint<void()>>> bridges;
    std::vector<ScriptHookEndpointDescriptor> endpoints;
    for (std::size_t index{}; index < count; ++index)
    {
        const HookPointId id{kHook.value + index};
        auto specification = makeHookPointSpec<void()>(kHook, "scale");
        specification.id = id;
        names.push_back("scale" + std::to_string(index));
        specification.diagnostic_name = names.back();
        specifications.push_back(specification);
        hooks.push_back(std::make_unique<HookPoint<void()>>());
        assert(hooks.back()->prepare(1U) == EEndpointMutationError::NONE);
        bridges.push_back(std::make_unique<ScriptHookEndpoint<void()>>(kSystem, id, *hooks.back()));
        endpoints.push_back(bridges.back()->descriptor());
        harness.description[index].bindings[0].target = HookScriptTarget{kSystem, id};
    }
    const SimulationSystemDescription description{
        .type = {.canonical_name = "lux.test.script-scale", .version = 1U}, .hooks = specifications
    };
    SimulationDescriptionBuilder builder;
    assert(builder.addSystem(kSystem, "scale", description));
    auto simulation = std::move(builder).build();
    assert(simulation);
    const auto plan = planScriptRuntimeCapacity(harness.description);
    assert(plan);
    auto created = ScriptSystem::create(*simulation, *plan, harness.description, harness.registry, harness.clock,
        ScriptRuntimeLimits{32U, count + 8U, 16U, 8U, 16U, 16U, 64U, 16U, 16U, 16U, 16U, 16U},
        {&harness, &Harness::resolveArtifact}, {}, std::span{&harness.backend, 1U}, endpoints, {});
    assert(created && created->prepare());
    auto& system = *created;
    std::vector<ScriptMountStatus> initial_feedback(count);
    assert(system.collectMountStatusChanges(initial_feedback)->written == count);
    assert(system.activeInstanceCount() == count);
    using Clock = std::chrono::steady_clock;
    std::int64_t elapsed{};
    std::size_t allocations{};
    std::uint64_t slot_visits{}, endpoint_visits{};
    const auto backing = system.stats();
    for (std::size_t iteration{}; iteration < 144U; ++iteration)
    {
        const auto before_stats = system.stats();
        allocation_accounting = diagnostics;
        allocation_count = 0U;
        const auto before = Clock::now();
        harness.registry.destroy(harness.entities[0]);
        harness.entities[0] = harness.registry.create();
        harness.submitEntity(system, 0U);
        assert(system.processLifecycle());
        const auto after = Clock::now();
        allocation_accounting = false;
        const auto after_stats = system.stats();
        assert(system.activeInstanceCount() == count);
        assert(after_stats.pending_mounts == 0U && after_stats.active_continuations == 0U);
        assert(after_stats.mount_backing_bytes == backing.mount_backing_bytes);
        assert(after_stats.method_backing_bytes == backing.method_backing_bytes);
        assert(after_stats.binding_backing_bytes == backing.binding_backing_bytes);
        assert(system.failures().empty());
        if (iteration >= 16U)
        {
            elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
            allocations += allocation_count;
            COUNTERS
        }
    }
    // All other configurations remained active; binding reconstruction preserved exactly one call per Hook.
    for (auto& hook : hooks) assert(dispatchHookForTest(*hook));
    assert(harness.backend_state.tick_calls == count);
    for (std::size_t index{count}; index < count + 144U; ++index)
        assert(harness.backend_state.hosts[index] == harness.backend_state.hosts[0]);
    assert(system.shutdown());
    assert(harness.backend_state.creates == count + 144U);
    assert(harness.backend_state.destroys == count + 144U);
    assert(harness.backend_state.begins == count + 144U);
    assert(harness.backend_state.ends == count + 144U);
    assert(harness.backend_state.prepares == (count + 144U) * 3U);
    assert(harness.backend_state.releases == harness.backend_state.prepares);
    std::printf("configs,endpoints,warmup,rebuilds,elapsed_ns,diagnostics,exe_allocations,slot_visits,"
        "endpoint_count_visits,counter_available,creates,destroys,ticks,errors,backlog\n");
    std::printf("%zu,%zu,16,128,%lld,%d,%zu,%llu,%llu,COUNTER_AVAILABLE,%zu,%zu,%zu,0,0\n", count, count,
        static_cast<long long>(elapsed), diagnostics, allocations,
        static_cast<unsigned long long>(slot_visits), static_cast<unsigned long long>(endpoint_visits),
        harness.backend_state.creates, harness.backend_state.destroys, harness.backend_state.tick_calls);
}
'''


def main():
    parser = argparse.ArgumentParser()
    for key in ('baseline-source', 'candidate-source', 'baseline-prefix', 'candidate-prefix', 'dependencies', 'output'):
        parser.add_argument('--' + key, required=True)
    args = parser.parse_args()
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    variants, manifest, records = {}, [], []
    for variant in ('baseline', 'candidate'):
        source_root = Path(getattr(args, variant + '_source'))
        prefix = getattr(args, variant + '_prefix')
        directory = root / variant
        directory.mkdir()
        fixture = source_root / 'engine/domain/simulation/builtin/script/test/script_system_lifecycle_test.cpp'
        INSTRUMENTATION_HITS.clear()
        source = '#include <string>\n' + truncate_main(
            instrument(fixture.read_text(encoding='utf-8-sig'), True), 'int main(int argc')
        stats = source_root / 'engine/domain/simulation/builtin/script/include/lux/engine/simulation/ScriptSystem.hpp'
        counters = 'assembly_configuration_slot_visits' in stats.read_text(encoding='utf-8-sig')
        main_source = MAIN.replace('COUNTERS', '''
            slot_visits += after_stats.assembly_configuration_slot_visits -
                before_stats.assembly_configuration_slot_visits;
            endpoint_visits += after_stats.assembly_endpoint_count_visits -
                before_stats.assembly_endpoint_count_visits;
''' if counters else '')
        source += main_source.replace('COUNTER_AVAILABLE', '1' if counters else '0')
        (directory / 'scale.cpp').write_text(source, encoding='utf-8')
        helper = source_root / 'engine/domain/simulation/system/test/HookInvocationTestAccess.hpp'
        (directory / helper.name).write_bytes(helper.read_bytes())
        (directory / 'instrumentation.json').write_text(json.dumps(INSTRUMENTATION_HITS, indent=2))
        (directory / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.22)
project(script_scale LANGUAGES CXX)
find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)
add_executable(script_scale scale.cpp)
target_compile_features(script_scale PRIVATE cxx_std_20)
target_compile_options(script_scale PRIVATE /UNDEBUG)
target_link_libraries(script_scale PRIVATE lux::engine::simulation::simulation_script)
''')
        run(['cmake', '-S', str(directory), '-B', str(directory / 'build'), '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
             '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
             '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
             '-DCMAKE_PREFIX_PATH=' + prefix + ';' + args.dependencies], directory / 'configure.log')
        command = ['cmake', '--build', str(directory / 'build'), '--target', 'all', '-j', '4', '--', '-k', '0']
        run(command, directory / 'build.log')
        run(command, directory / 'second-build.log')
        exe = directory / 'build/script_scale.exe'
        env = dict(os.environ)
        clean = [p for p in env['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]
        env['PATH'] = ';'.join([str(Path(prefix) / 'bin'), 'D:/Development/vcpkg/installed/x64-windows/bin'] +
                               [str(Path(p) / 'bin') for p in args.dependencies.split(';')] + clean)
        variants[variant] = exe, env
        manifest.append({'variant': variant, 'source_commit': subprocess.check_output(
            ['git', '-C', str(source_root), 'rev-parse', 'HEAD'], text=True).strip(),
            'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
            'generated_sha256': hashlib.sha256(source.encode()).hexdigest(),
            'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'prefix': prefix, 'counters': counters})
    def execute(variant, size, mode, log):
        exe, env = variants[variant]
        run([str(exe), str(size), mode], log, env)
        rows = list(csv.DictReader(log.read_text().splitlines()[1:]))
        if len(rows) != 1:
            raise RuntimeError('Missing or duplicate scale business row: ' + str(log))
        row = {key: int(value) for key, value in rows[0].items()}
        expected = {'configs': size, 'endpoints': size, 'warmup': 16, 'rebuilds': 128,
                    'creates': size + 144, 'destroys': size + 144, 'ticks': size, 'errors': 0, 'backlog': 0,
                    'diagnostics': int(mode == 'diagnostics')}
        if any(row[key] != value for key, value in expected.items()) or row['elapsed_ns'] <= 0:
            raise RuntimeError('Invalid scale business result: ' + str(log))
        records.append({'variant': variant, 'log': log.name, **row})
        (root / 'runs.json').write_text(json.dumps(records, indent=2))

    for size in (8, 8192):
        for pair in range(5):
            for variant in (('baseline', 'candidate') if pair % 2 == 0 else ('candidate', 'baseline')):
                execute(variant, size, 'timing', root / f'{variant}-{size}-{pair}.csv.log')
        for variant in variants:
            execute(variant, size, 'diagnostics', root / f'{variant}-{size}-diagnostics.csv.log')
    (root / 'manifest.json').write_text(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
