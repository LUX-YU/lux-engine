"""Build the same observed FlowForge benchmark source against each fixed installed runtime.

Observations run outside timing. Uses the existing installed-consumer CMake path and qualified
private native projection; creates no runtime API, instrumented DLL, or alternative scheduler.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from RunScriptSR2Probes import run


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for name in ('source', 'runtime-source', 'prefix', 'dependencies', 'output'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    prefix = Path(args.prefix)
    source = Path(args.source) / 'test/runtime_scripting/flowforge_script_runtime_integration_test.cpp'
    runtime_sha = subprocess.check_output(['git', '-C', args.runtime_source, 'rev-parse', 'HEAD'], text=True).strip()
    if subprocess.check_output(['git', '-C', args.runtime_source, 'status', '--porcelain'], text=True).strip():
        raise RuntimeError('Runtime reference must be a clean tracked source')
    (root / 'probe.cpp').write_bytes(source.read_bytes())
    generated = prefix.parent.parent.parent / 'build/RelWithDebInfo' / prefix.parent.name / prefix.name / (
        'engine/domain/simulation/builtin/script/generated/simulation_script/script_abilities')
    headers = {}
    for header in generated.glob('DelayAbility.*.hpp'):
        (root / header.name).write_bytes(header.read_bytes())
        headers[str(header)] = digest(header)
    if len(headers) != 3:
        raise RuntimeError('Expected the three qualified native projection headers')
    cmake = '''cmake_minimum_required(VERSION 3.22)
project(script_event_observer LANGUAGES CXX)
find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script simulation_script_native)
find_package(lux-engine-core REQUIRED COMPONENTS task)
find_package(lux-engine-scene-script-description REQUIRED COMPONENTS scene_script_description)
add_executable(script_event_observer probe.cpp)
target_compile_features(script_event_observer PRIVATE cxx_std_20)
target_compile_options(script_event_observer PRIVATE /UNDEBUG)
target_link_libraries(script_event_observer PRIVATE lux::engine::flowforge::flowforge_compiler
    lux::engine::simulation::simulation_composition lux::engine::simulation::simulation_script
    lux::engine::simulation::simulation_script_native lux::engine::core::task
    lux::engine::scene::scene_script_description)
'''
    cmake += ('target_compile_definitions(script_event_observer PRIVATE '
              f'LUX_BENCHMARK_GIT_COMMIT="{runtime_sha}" LUX_BENCHMARK_BUILD_TYPE="RelWithDebInfo")\n')
    (root / 'CMakeLists.txt').write_text(cmake)
    run(['cmake', '-S', str(root), '-B', str(root / 'build'), '-G', 'Ninja',
         '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
         '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
         '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
         '-DCMAKE_PREFIX_PATH=' + str(prefix) + ';' + args.dependencies], root / 'configure.log')
    for log in ('build.log', 'second-build.log'):
        run(['cmake', '--build', str(root / 'build'), '--target', 'all', '-j', '4', '--', '-k', '0'], root / log)
    if 'no work to do' not in (root / 'second-build.log').read_text():
        raise RuntimeError('Second observer build was not a no-op')
    exe = root / 'build/script_event_observer.exe'
    (root / 'identity.json').write_text(json.dumps(dict(runtime_sha=runtime_sha, source=str(source),
        source_sha256=digest(source), headers=headers, executable=str(exe), executable_sha256=digest(exe),
        dll_sha256=digest(prefix / 'bin/lux_engine_simulation_script.dll')), indent=2))
    print(exe)


if __name__ == '__main__':
    main()
