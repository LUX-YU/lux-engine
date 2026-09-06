"""Supplement the existing FlowForge benchmark with executable-local allocation diagnostics.

DLL-wide heap accounting is not provided by a replacement executable operator new on Windows.
These runs are separate from the five paired performance processes.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
from RunScriptSR2Probes import run


def main():
    parser = argparse.ArgumentParser()
    for name in ("baseline-source", "candidate-source", "baseline-prefix", "candidate-prefix", "dependencies", "output"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    records = []
    for variant in ("baseline", "candidate"):
        source_root = Path(getattr(args, variant + "_source"))
        prefix = Path(getattr(args, variant + "_prefix"))
        directory = root / variant
        directory.mkdir()
        # This internal fixture includes the qualified native projection, which is not a public SDK header.
        generated = prefix.parent.parent.parent / "build/RelWithDebInfo" / prefix.parent.name / prefix.name / (
            "engine/domain/simulation/builtin/script/generated/simulation_script/script_abilities")
        generated_identity = {}
        for header in generated.glob("DelayAbility.*.hpp"):
            (directory / header.name).write_bytes(header.read_bytes())
            generated_identity[str(header)] = hashlib.sha256(header.read_bytes()).hexdigest()
        source = (source_root / "test/runtime_scripting/flowforge_script_runtime_integration_test.cpp").read_text(
            encoding="utf-8-sig")
        source = '''#include <atomic>
#include <cstdlib>
#include <new>
std::atomic_bool allocation_accounting{};
std::atomic_size_t executable_allocations{};
void* operator new(std::size_t size)
{
    if (allocation_accounting.load(std::memory_order_relaxed))
        executable_allocations.fetch_add(1U, std::memory_order_relaxed);
    if (auto* value = std::malloc(size ? size : 1U)) return value;
    throw std::bad_alloc{};
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
''' + source
        source = source.replace('completed,phase\\n', 'completed,phase,executable_allocations\\n')
        source = source.replace('<< (draining ? "drain" : "steady") << \'\\n\';',
                                '<< (draining ? "drain" : "steady") << \',\' << executable_allocations.load() << \'\\n\';')
        source = source.replace('                const auto begin = Clock::now();',
                                '                executable_allocations = 0U; allocation_accounting = true;\n'
                                '                const auto begin = Clock::now();')
        source = source.replace('                if (!success)',
                                '                allocation_accounting = false;\n                if (!success)')
        (directory / "probe.cpp").write_text(source)
        cmake = '''cmake_minimum_required(VERSION 3.22)
project(sr2_flow_allocations LANGUAGES CXX)
find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script simulation_script_native)
find_package(lux-engine-core REQUIRED COMPONENTS task)
add_executable(sr2_flow_allocations probe.cpp)
target_compile_features(sr2_flow_allocations PRIVATE cxx_std_20)
target_compile_options(sr2_flow_allocations PRIVATE /UNDEBUG)
target_link_libraries(sr2_flow_allocations PRIVATE lux::engine::flowforge::flowforge_compiler
    lux::engine::simulation::simulation_composition lux::engine::simulation::simulation_script
    lux::engine::simulation::simulation_script_native lux::engine::core::task)
'''
        if variant == "candidate":
            cmake += '''find_package(lux-engine-scene-script-description REQUIRED COMPONENTS scene_script_description)
target_link_libraries(sr2_flow_allocations PRIVATE lux::engine::scene::scene_script_description)
'''
        (directory / "CMakeLists.txt").write_text(cmake)
        run(["cmake", "-S", str(directory), "-B", str(directory / "build"), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
             "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
             "-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake",
             "-DCMAKE_PREFIX_PATH=" + str(prefix) + ";" + args.dependencies], directory / "configure.log")
        for log in ("build.log", "second-build.log"):
            run(["cmake", "--build", str(directory / "build"), "--target", "all", "-j", "4", "--", "-k", "0"],
                directory / log)
        env = dict(os.environ)
        env["PATH"] = ";".join([str(prefix / "bin"), "D:/Development/vcpkg/installed/x64-windows/bin"] +
                               [p for p in env["PATH"].split(";") if "CodeRepos" not in p and "vcpkg" not in p])
        for case in ("scene-flowforge-update-heavy", "scene-flowforge-event"):
            output = directory / (case + ".csv")
            exe = directory / "build/sr2_flow_allocations.exe"
            run([str(exe), "--group", case, "--mode", "diagnostic", "--size", "10000", "--warmups", "1",
                 "--frames", "3", "--seed", "1592598566", "--resume-budget", "2000", "--output", str(output)],
                directory / (case + ".log"), env)
            rows = list(csv.DictReader(output.open()))
            assert len(rows) == 3 and "executable_allocations" in rows[0]
            records.append(dict(variant=variant, case=case, executable_allocations=[r["executable_allocations"] for r in rows],
                                qualified_native_projection=generated_identity,
                                source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                                executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest()))
    (root / "manifest.json").write_text(json.dumps(records, indent=2))


if __name__ == "__main__":
    main()
