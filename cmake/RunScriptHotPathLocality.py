"""Independent order diagnostic using the installed three-language authoring consumer.

No production dispatch order changes. Each object only increments its own sum; the shared
observations are bounded integer additions. Run serially from a VS development shell.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    for key in ('source', 'prefix', 'dependencies', 'output'):
        parser.add_argument('--' + key, required=True)
    args = parser.parse_args()
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    source = root / 'source'
    shutil.copytree(Path(args.source) / 'cmake/installed-consumers/script-authoring', source)
    text = (source / 'main.cpp').read_text(encoding='utf-8-sig')
    hits = []

    def replace(old, new):
        nonlocal text
        count = text.count(old)
        assert count == 1, (old, count)
        hits.append(dict(fragment=old, hits=count))
        text = text.replace(old, new)

    replace('#include <array>', '#include <array>\n#include <chrono>')
    replace('need(argc == 3, "arguments: save|load|reject binding-file");',
            'need(argc == 4, "arguments: save binding-file interleaved|grouped");\n'
            '        const bool grouped = std::string_view{argv[3]} == "grouped";\n'
            '        need(grouped || std::string_view{argv[3]} == "interleaved", "order");\n'
            '        constexpr std::size_t per_language = 256U, population = per_language * 3U;')
    replace('generated::CommonBehavior, 1U, 0U, 0U,', 'generated::CommonBehavior, per_language, 0U, 0U,')
    replace('alignof(std::max_align_t), 1U}};', 'alignof(std::max_align_t), per_language}};')
    replace('LuaPreparedBlockClass{1U, 1U}', 'LuaPreparedBlockClass{1U, per_language}')
    replace('LuaScriptBackend::create({.instance_capacity = 1U, .prepared_call_capacity = 1U,',
            'LuaScriptBackend::create({.instance_capacity = per_language, .prepared_call_capacity = per_language,')
    replace('.prepared_ability_capacity = 1U,', '.prepared_ability_capacity = per_language,')
    replace('.prepared_ability_storage_bytes = 4096U', '.prepared_ability_storage_bytes = per_language * 4096U')
    replace('NativeScriptStoragePopulation{&*module, 1U, 0U}',
            'NativeScriptStoragePopulation{&*module, per_language, 0U}')
    # The second occurrence is now uniquely the Native configuration.
    replace('.instance_capacity = 1U, .prepared_call_capacity = 1U,',
            '.instance_capacity = per_language, .prepared_call_capacity = per_language,')
    replace('.state_storage_bytes = 4096U', '.state_storage_bytes = per_language * 4096U')
    replace('auto runtime = ScriptSystem::create(', '''const auto initial = resolveScriptRuntimeMounts(
            *mount_description, {&sources, &Sources::world}, registry);
        need(static_cast<bool>(initial), "resolve initial");
        std::vector<ScriptRuntimeMount> inputs;
        for (std::size_t i{}; i < population; ++i)
        {
            const auto language = grouped ? i / per_language : i % 3U;
            auto input = (*initial)[language];
            input.id = ScriptMountId{i + 1U};
            input.scope = EntityScriptScope{registry.create()};
            input.configuration_index = static_cast<std::uint32_t>(i);
            inputs.push_back(std::move(input));
        }
        ScriptRuntimeCapacityPlan capacity{population, population, population, population * 3U,
            {{HookScriptTarget{HostId, Selected}, population}}};
        auto runtime = ScriptSystem::create(''')
    replace('*planScriptRuntimeCapacity(*mount_description),', 'capacity,')
    replace('*resolveScriptRuntimeMounts(*mount_description, {&sources, &Sources::world}, registry),', 'inputs,')
    replace('{16U, 3U, 8U, 8U, 8U, 8U, 64U, 8U, 8U, 8U, 8U, 8U}',
            '{16U, population, 8U, 8U, 8U, 8U, 64U, 8U, 8U, 8U, 8U, 8U}')
    replace('for (unsigned step{}; step < 3U; ++step)\n'
            '            need(static_cast<bool>(simulation->execute(*executor, SimulationDuration{1})), "execute graph");\n'
            '        need(authoring_consumer::cpp_total == 30 && provider_total == 60 && provider_calls == 6U, "observations");',
            '''std::array<std::int64_t, 512U> samples{};
        for (unsigned step{}; step < 528U; ++step)
        {
            const auto before = std::chrono::steady_clock::now();
            need(static_cast<bool>(simulation->execute(*executor, SimulationDuration{1})), "execute graph");
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - before).count();
            if (step >= 16U) samples[step - 16U] = elapsed;
        }
        constexpr auto total = per_language * 5U * 528U * 529U / 2U;
        need(authoring_consumer::cpp_total == total && provider_total == 2U * total &&
            provider_calls == 2U * per_language * 528U, "observations");
        need(runtime->failures().empty(), "errors");
        std::cout << "LOCALITY order=" << argv[3] << " population=" << population
            << " measured_calls=" << population * 512U << " provider_calls=" << provider_calls
            << " cpp_total=" << authoring_consumer::cpp_total << " provider_total=" << provider_total
            << " errors=0 backlog=0\\n";
        for (const auto value : samples) std::cout << "BATCH " << value << '\\n';''')
    replace('three source frontends -> selected saved bindings -> graph: cpp=30 provider=60 calls=6',
            'LOCALITY_CLEANUP PASS')
    (source / 'main.cpp').write_text(text, encoding='utf-8')
    (root / 'instrumentation.json').write_text(json.dumps(hits, indent=2))
    env = dict(os.environ)
    clean = [p for p in env['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]
    prefixes = (args.prefix + ';' + args.dependencies).split(';')
    env['PATH'] = ';'.join([str(Path(p) / 'bin') for p in prefixes] +
                           ['D:/Development/vcpkg/installed/x64-windows/bin'] + clean)
    runs = []

    def run(command, name):
        with (root / name).open('w') as log:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        runs.append(dict(command=command, log=name, exit=result.returncode))
        (root / 'runs.json').write_text(json.dumps(runs, indent=2))
        assert result.returncode == 0, name

    build = root / 'build'
    run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
         '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
         '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF', '-DVCPKG_MANIFEST_MODE=OFF',
         '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
         '-DCMAKE_PREFIX_PATH=' + args.prefix + ';' + args.dependencies], 'configure.log')
    command = ['cmake', '--build', str(build), '--target', 'all', '-j', '4', '--', '-k', '0']
    run(command, 'build.log')
    run(command, 'noop.log')
    exe = build / 'lux_script_authoring_consumer.exe'
    (root / 'identity.json').write_text(json.dumps(dict(
        source=subprocess.check_output(['git', '-C', args.source, 'rev-parse', 'HEAD'], text=True).strip(),
        driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        generated_sha256=hashlib.sha256(text.encode()).hexdigest(),
        exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest()), indent=2))
    for pair in range(5):
        for mode in (('interleaved', 'grouped') if pair % 2 == 0 else ('grouped', 'interleaved')):
            name = f'{pair}-{mode}.log'
            run([str(exe), 'save', str(root / 'bindings.bin'), mode], name)
            result = (root / name).read_text()
            assert 'LOCALITY_CLEANUP PASS' in result and result.count('BATCH ') == 512, name


if __name__ == '__main__':
    main()
