"""Build identical lifecycle observations against two separately installed SR-2 snapshots.

Run from the VS development environment. Source copies are diagnostic drivers only;
neither reference checkout is edited. All commands, generated sources and raw output survive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


LIFECYCLE_TRACE_COUNTS = {
    "initial": {
        "create": 3,
        "prepare": 9,
        "invoke": 9,
        "release": 9,
        "destroy": 3
    },
    "failures": {
        "create": 8,
        "prepare": 20,
        "release": 20,
        "destroy": 8,
        "invoke": 10
    },
    "signatures": {
        "signature-rejected": 2
    },
    "incarnations": {
        "create": 4,
        "prepare": 12,
        "invoke": 8,
        "continuation-destroy": 2,
        "release": 12,
        "destroy": 4
    },
    "pending-fatal": {
        "create": 5,
        "prepare": 15,
        "invoke": 13,
        "release": 15,
        "destroy": 5
    },
    "cross-batch": {
        "create": 4,
        "prepare": 12,
        "invoke": 18,
        "release": 12,
        "destroy": 4
    }
}

EVENT_TRACE_COUNTS = {
    "TargetedAndRetirement": {
        "create": 2,
        "step": 6,
        "resume": 2,
        "continuation-destroy": 3,
        "release": 4,
        "destroy": 2
    },
    "RegistrationCutoff": {
        "create": 1,
        "step": 3,
        "resume": 2,
        "continuation-destroy": 2,
        "release": 2,
        "destroy": 1
    },
    "NestedDispatch": {
        "create": 1,
        "step": 4,
        "resume": 1,
        "continuation-destroy": 1,
        "release": 2,
        "destroy": 1
    },
    "PreparedAdmissionProvenance": {
        "create": 4,
        "step": 3,
        "resume": 1,
        "continuation-destroy": 1,
        "release": 4,
        "destroy": 4
    },
    "CopyRetirementPin": {
        "create": 1,
        "step": 1,
        "continuation-destroy": 1,
        "release": 1,
        "destroy": 1
    },
    "CopyOtherRecordRemoval": {
        "create": 2,
        "step": 3,
        "continuation-destroy": 2,
        "release": 3,
        "destroy": 2,
        "resume": 1
    },
    "CopyShutdownAndFailure": {
        "create": 2,
        "step": 2,
        "continuation-destroy": 2,
        "release": 2,
        "destroy": 2
    },
    "CopyNestedAdmission": {
        "create": 4,
        "step": 7,
        "resume": 4,
        "continuation-destroy": 5,
        "release": 6,
        "destroy": 4
    }
}

INSTRUMENTATION_HITS = []


def replace_exact(source, old, new, count=1):
    actual = source.count(old)
    INSTRUMENTATION_HITS.append({"token": old, "expected": count, "actual": actual})
    if actual != count:
        raise RuntimeError(f"Instrumentation expected {count} hits, found {actual}: {old!r}")
    return source.replace(old, new)


def truncate_main(source, marker):
    if source.count(marker) != 1:
        raise RuntimeError("Expected exactly one fixture main: " + marker)
    return source[:source.index(marker)]


def check_trace(lines, expected):
    cases = {}
    current = None
    for line in lines:
        if line.startswith("CASE "):
            current = line[5:]
            if current in cases:
                raise RuntimeError("Duplicate trace case: " + current)
            cases[current] = {}
        elif line:
            if current is None:
                raise RuntimeError("Business event outside a trace case: " + line)
            event = line.split(",")[0]
            cases[current][event] = cases[current].get(event, 0) + 1
    if list(cases) != list(expected) or cases != expected:
        raise RuntimeError("Trace coverage/count mismatch: " + json.dumps(cases))
    return cases


def run(command, log, env=None):
    with log.open("w", encoding="utf-8") as stream:
        stream.write(json.dumps(command) + "\n")
        stream.flush()
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, env=env)
    if result.returncode:
        raise RuntimeError(f"{log}: exit {result.returncode}")


def instrument(source, candidate):
    source = replace_exact(source,'"../../../system/test/HookInvocationTestAccess.hpp"',
                            '"HookInvocationTestAccess.hpp"')
    source = '''#include <cstdio>
#include <chrono>
#include <string_view>
#include <cstdlib>
#include <new>
#include <malloc.h>
bool allocation_accounting{};
std::size_t allocation_count{};
void* operator new(std::size_t size)
{
    if (allocation_accounting) ++allocation_count;
    if (auto* value = std::malloc(size ? size : 1U)) return value;
    throw std::bad_alloc{};
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    if (allocation_accounting) ++allocation_count;
    if (auto* value = _aligned_malloc(size ? size : 1U, static_cast<std::size_t>(alignment))) return value;
    throw std::bad_alloc{};
}
void operator delete(void* value, std::align_val_t) noexcept { _aligned_free(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept { _aligned_free(value); }
''' + source
    source = replace_exact(source,"namespace\n{", """namespace
{
    bool trace_enabled{};
    void trace(const char* event, std::size_t instance, std::uint64_t symbol = 0U) noexcept
    {
        if (trace_enabled)
            std::printf("%s,%zu,%llu\\n", event, instance, static_cast<unsigned long long>(symbol));
    }
""", 1)
    if not candidate:
        source = replace_exact(source,"        std::vector<ScriptAwaitableCompletion> completions;",
                                "        std::vector<ScriptAwaitableCompletion> completions;\n"
                                "        std::vector<ScriptBehavior*> hosts;")
        source = replace_exact(source,"        const ScriptInstanceCreateContext&,",
                                "        const ScriptInstanceCreateContext& create_context,")
        source = replace_exact(source,"        ++state.creates;\n        output.value = instance;",
                                "        ++state.creates;\n        state.hosts.push_back(create_context.behavior);\n"
                                "        output.value = instance;")
    source = replace_exact(source,"        ++state.creates;", '        ++state.creates;\n        trace("create", state.creates);')
    source = replace_exact(source,"        ++state.prepares;", '        ++state.prepares;\n'
                            '        trace("prepare", static_cast<Instance*>(instance.value)->serial, function.symbol_id);')
    source = replace_exact(source,"        ++static_cast<BackendState*>(context)->releases;",
                            '        trace("release", static_cast<PreparedCall*>(method.token)->instance->serial,\n'
                            '            static_cast<PreparedCall*>(method.token)->symbol);\n'
                            '        ++static_cast<BackendState*>(context)->releases;')
    source = replace_exact(source,"        ++static_cast<BackendState*>(context)->destroys;",
                            '        trace("destroy", static_cast<Instance*>(instance.value)->serial);\n'
                            '        ++static_cast<BackendState*>(context)->destroys;')
    source = replace_exact(source,"        auto& state = *instance.owner;",
                            '        auto& state = *instance.owner;\n        trace("invoke", instance.serial, call.symbol);')
    source = replace_exact(source,"        ++continuation->owner->continuation_destroys;",
                            '        trace("continuation-destroy", continuation->instance->serial);\n'
                            '        ++continuation->owner->continuation_destroys;')
    for variable in ("begin_prepared", "end_prepared"):
        assertion = f"        assert(!{variable} && {variable}.error() == EScriptSystemError::SIGNATURE_MISMATCH);"
        source = replace_exact(source, assertion,
            assertion + '\n        if (trace_enabled) std::puts("signature-rejected");')
    source = truncate_main(source, "int main()")
    cross_batch = """
void testCrossBatchOrder()
{
    Harness harness{3U, true, true, true};
    harness.registry.destroy(harness.entities[1]);
    harness.entities[1] = NullEntity;
    INITIAL_CROSS_BATCH
    assert(created && created->prepare());
    auto& system = *created;
    assert(dispatchHookForTest(harness.hook) == 1U);
    harness.entities[1] = harness.registry.create();
    SUBMIT_SECOND
    assert(system.processLifecycle());
    assert(dispatchHookForTest(harness.hook) == 1U);
    harness.registry.destroy(harness.entities[2]);
    harness.entities[2] = harness.registry.create();
    SUBMIT_THIRD
    assert(system.processLifecycle());
    assert(dispatchHookForTest(harness.hook) == 1U);
    assert(system.shutdown());
    assert(harness.backend_state.creates == 4U && harness.backend_state.destroys == 4U);
}
"""
    cross_batch = replace_exact(cross_batch,"INITIAL_CROSS_BATCH", """
    std::swap(harness.description[1], harness.description[2]);
    auto created = harness.create(2U);
    std::swap(harness.description[1], harness.description[2]);
""" if candidate else "auto created = harness.create();")
    cross_batch = replace_exact(cross_batch,"SUBMIT_SECOND", "harness.submitEntity(system, 1U);" if candidate else "")
    cross_batch = replace_exact(cross_batch,"SUBMIT_THIRD", "harness.submitEntity(system, 2U);" if candidate else "")
    source += cross_batch
    source += """
int main(int argc, char** argv)
{
    if (argc == 1)
    {
        trace_enabled = true;
        std::puts("CASE initial"); testInitialLifecycle();
        std::puts("CASE failures"); testOptionalAndFailureLifecycle();
        std::puts("CASE signatures"); testSignatureValidation();
        std::puts("CASE incarnations"); testIncarnationAndPendingContinuation();
        std::puts("CASE pending-fatal"); testPendingDoesNotMaskFatal();
        std::puts("CASE cross-batch"); testCrossBatchOrder();
        return 0;
    }
    using Clock = std::chrono::steady_clock;
    Harness harness{8U, true, true, true};
    for (std::size_t index{1U}; index < 8U; ++index)
    {
        harness.registry.destroy(harness.entities[index]);
        harness.entities[index] = NullEntity;
    }
    allocation_accounting = std::string_view{argv[1]} == "--allocations";
    allocation_count = 0U;
    const auto start = Clock::now();
    auto created = harness.create(INITIAL_COUNT);
    assert(created && created->prepare());
    auto& system = *created;
    const auto prepared = Clock::now();
    const auto prepare_allocations = allocation_count;
    for (std::size_t index{1U}; index < 8U; ++index)
        harness.entities[index] = harness.registry.create();
    allocation_count = 0U;
    const auto late_start = Clock::now();
    SUBMIT_LATE
    assert(system.processLifecycle());
    const auto late_end = Clock::now();
    const auto late_allocations = allocation_count;
    assert(system.activeInstanceCount() == 8U);
    std::int64_t remount_ns{};
    std::size_t remount_allocations{};
    for (std::size_t iteration{}; iteration < 144U; ++iteration)
    {
        allocation_count = 0U;
        const auto before = Clock::now();
        harness.registry.destroy(harness.entities[0]);
        harness.entities[0] = harness.registry.create();
        SUBMIT_REPLACEMENT
        assert(system.processLifecycle());
        const auto after = Clock::now();
        if (iteration >= 16U)
        {
            remount_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
            remount_allocations += allocation_count;
        }
    }
    assert(system.shutdown());
    assert(harness.backend_state.creates == 152U);
    assert(harness.backend_state.destroys == 152U);
    assert(harness.backend_state.prepares == harness.backend_state.releases);
    std::printf("prepare_ns,late_ns,remount_128_ns,creates,destroys,prepares,releases\\n%lld,%lld,%lld,%zu,%zu,%zu,%zu\\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(prepared - start).count()),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(late_end - late_start).count()),
        static_cast<long long>(remount_ns), harness.backend_state.creates, harness.backend_state.destroys,
        harness.backend_state.prepares, harness.backend_state.releases);
    std::printf("allocation_accounting,prepare_allocations,late_allocations,remount_128_allocations\\n%d,%zu,%zu,%zu\\n",
        allocation_accounting, prepare_allocations, late_allocations, remount_allocations);
    return 0;
}
"""
    source = replace_exact(source,"INITIAL_COUNT", "1U" if candidate else "")
    source = replace_exact(source,"SUBMIT_LATE", """
    std::array<ScriptMountStatus, 8U> feedback;
    assert(system.collectMountStatusChanges(feedback));
    for (std::size_t index{1U}; index < 8U; ++index)
        harness.description[index].scope = EntityScriptScope{harness.entities[index]};
    assert(system.mountResolvedBatch(std::span{harness.description}.subspan(1U)));
""" if candidate else "")
    source = replace_exact(source,"SUBMIT_REPLACEMENT", "harness.submitEntity(system, 0U);" if candidate else "")
    return source


def instrument_events(source):
    source = replace_exact(source,'"../../../scripting/core/test/ScriptEndpointTestAccess.hpp"',
                            '"ScriptEndpointTestAccess.hpp"')
    replacements = {
        "        ++continuation->owner->continuation_destroys;":
            '        std::puts("continuation-destroy");\n        ++continuation->owner->continuation_destroys;',
        "        state.resume_values.push_back(value);":
            '        std::printf("resume,%d\\n", value);\n        state.resume_values.push_back(value);',
        "        ++state.step_calls;":
            '        std::printf("step,%llu,%u\\n", static_cast<unsigned long long>(call.symbol),\n'
            '            static_cast<std::uint32_t>(call.instance->self));\n        ++state.step_calls;',
        "        output.value = instance;":
            '        std::printf("create,%u\\n", static_cast<std::uint32_t>(instance->self));\n'
            '        output.value = instance;',
        "        delete static_cast<PreparedCall*>(method.token);":
            '        std::printf("release,%llu\\n", static_cast<unsigned long long>(\n'
            '            static_cast<PreparedCall*>(method.token)->symbol));\n'
            '        delete static_cast<PreparedCall*>(method.token);',
        "        delete static_cast<BackendInstance*>(instance.value);":
            '        std::printf("destroy,%u\\n", static_cast<std::uint32_t>(\n'
            '            static_cast<BackendInstance*>(instance.value)->self));\n'
            '        delete static_cast<BackendInstance*>(instance.value);',
    }
    for old, new in replacements.items():
        if old not in source:
            raise RuntimeError("Event trace instrumentation no longer matches fixture: " + old)
        source = replace_exact(source,old, new)
    source = truncate_main(source, "int main(")
    source += "int main()\n{\n"
    for test in ("TargetedAndRetirement", "RegistrationCutoff", "NestedDispatch", "PreparedAdmissionProvenance",
                 "CopyRetirementPin", "CopyOtherRecordRemoval", "CopyShutdownAndFailure", "CopyNestedAdmission"):
        source += f'    std::puts("CASE {test}"); test{test}();\n'
    return source + "    return 0;\n}\n"


def main():
    parser = argparse.ArgumentParser()
    for name in ("baseline-source", "candidate-source", "baseline-prefix", "candidate-prefix",
                 "dependencies", "output"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    manifest = []
    variants = {}
    for variant in ("baseline", "candidate"):
        source_root = Path(getattr(args, variant + "_source"))
        prefix = getattr(args, variant + "_prefix")
        directory = root / variant
        directory.mkdir()
        original = source_root / "engine/domain/simulation/builtin/script/test/script_system_lifecycle_test.cpp"
        INSTRUMENTATION_HITS.clear()
        original_text = original.read_text(encoding="utf-8-sig")
        runtime_input = "std::vector<ScriptRuntimeMount>" in original_text
        probe = instrument(original_text, runtime_input)
        (directory / "probe.cpp").write_text(probe, encoding="utf-8")
        helper = source_root / "engine/domain/simulation/system/test/HookInvocationTestAccess.hpp"
        (directory / helper.name).write_bytes(helper.read_bytes())
        event_test = original.with_name("script_system_event_wait_test.cpp")
        (directory / "events.cpp").write_text(instrument_events(event_test.read_text(encoding="utf-8-sig")))
        event_helper = source_root / "engine/domain/simulation/scripting/core/test/ScriptEndpointTestAccess.hpp"
        (directory / event_helper.name).write_bytes(event_helper.read_bytes())
        wire_path = source_root / ("engine/scene/integration/script_description/test/script_system_description_test.cpp"
                                   if runtime_input else
                                   "engine/domain/simulation/builtin/script/test/script_system_description_test.cpp")
        golden_header = wire_path.parent / "ScriptSystemV1Golden.hpp"
        if golden_header.exists():
            (directory / golden_header.name).write_bytes(golden_header.read_bytes())
        wire = '#include <fstream>\n' + wire_path.read_text(encoding="utf-8-sig")
        wire = replace_exact(wire,'int main()', 'int main(int argc, char** argv)')
        wire = replace_exact(wire,'    assert(encoded);', '    assert(encoded);\n'
                            '    assert(argc == 2);\n'
                            '    std::ofstream wire_output(argv[1], std::ios::binary);\n'
                            '    wire_output.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());\n'
                            '    assert(wire_output.good());\n')
        (directory / "wire.cpp").write_text(wire, encoding="utf-8")
        (directory / "instrumentation.json").write_text(json.dumps(INSTRUMENTATION_HITS, indent=2))
        (directory / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.22)
project(sr2_probe LANGUAGES CXX)
find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)
add_executable(sr2_probe probe.cpp)
target_compile_features(sr2_probe PRIVATE cxx_std_20)
target_compile_options(sr2_probe PRIVATE /UNDEBUG)
target_link_libraries(sr2_probe PRIVATE lux::engine::simulation::simulation_script)
add_executable(sr2_events events.cpp)
target_compile_features(sr2_events PRIVATE cxx_std_20)
target_compile_options(sr2_events PRIVATE /UNDEBUG)
target_link_libraries(sr2_events PRIVATE lux::engine::simulation::simulation_script)
''', encoding="utf-8")
        with (directory / "CMakeLists.txt").open("a", encoding="utf-8") as cmake:
            cmake.write("\nadd_executable(sr2_wire wire.cpp)\n"
                        "target_compile_features(sr2_wire PRIVATE cxx_std_20)\n"
                        "target_compile_options(sr2_wire PRIVATE /UNDEBUG)\n")
            if runtime_input:
                cmake.write("find_package(lux-engine-scene-script-description REQUIRED COMPONENTS scene_script_description)\n"
                            "target_link_libraries(sr2_wire PRIVATE lux::engine::scene::scene_script_description)\n")
            else:
                cmake.write("target_link_libraries(sr2_wire PRIVATE lux::engine::simulation::simulation_script)\n")
        run(["cmake", "-S", str(directory), "-B", str(directory / "build"), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
             "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
             "-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake",
             "-DCMAKE_PREFIX_PATH=" + prefix + ";" + args.dependencies], directory / "configure.log")
        run(["cmake", "--build", str(directory / "build"), "--target", "all", "-j", "4", "--", "-k", "0"],
            directory / "build.log")
        exe = directory / "build/sr2_probe.exe"
        env = dict(os.environ)
        clean_path = [p for p in env["PATH"].split(";") if "CodeRepos" not in p and "vcpkg" not in p]
        env["PATH"] = ";".join([str(Path(prefix) / "bin"), "D:/Development/vcpkg/installed/x64-windows/bin"] +
                               [str(Path(p) / "bin") for p in args.dependencies.split(";")] + clean_path)
        run([str(exe)], directory / "trace.log", env)
        run([str(directory / "build/sr2_events.exe")], directory / "event-trace.log", env)
        run([str(directory / "build/sr2_wire.exe"), str(directory / "v1.bin")], directory / "wire.log", env)
        run(["cmake", "--build", str(directory / "build"), "--target", "all", "-j", "4", "--", "-k", "0"],
            directory / "second-build.log")
        variants[variant] = (exe, env)
        manifest.append({"variant": variant, "source": str(original),
                         "source_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                         "driver_sha256": hashlib.sha256(probe.encode()).hexdigest(),
                         "executable_sha256": hashlib.sha256(exe.read_bytes()).hexdigest()})
    for pair in range(5):
        for variant in (("baseline", "candidate") if pair % 2 == 0 else ("candidate", "baseline")):
            exe, env = variants[variant]
            run([str(exe), "--measure"], root / f"{variant}-{pair}.csv.log", env)
    for variant, (exe, env) in variants.items():
        run([str(exe), "--allocations"], root / f"{variant}-allocations.csv.log", env)
    traces = [(root / variant / "trace.log").read_text().splitlines()[1:] for variant in variants]
    lifecycle_coverage = [check_trace(trace, LIFECYCLE_TRACE_COUNTS) for trace in traces]
    identical = traces[0] == traces[1]
    event_traces = [(root / variant / "event-trace.log").read_text().splitlines()[1:] for variant in variants]
    event_coverage = [check_trace(trace, EVENT_TRACE_COUNTS) for trace in event_traces]
    event_equal = event_traces[0] == event_traces[1]
    wire_bytes = [(root / variant / "v1.bin").read_bytes() for variant in variants]
    for data in wire_bytes:
        if len(data) != 288 or hashlib.sha256(data).hexdigest() != (
                "8002ffd5678f822d79949b4d0a36d1687613216af4b26b876f967fa24cc76e52"):
            raise RuntimeError("Persistent v1 golden size/hash mismatch")
    wire_equal = wire_bytes[0] == wire_bytes[1]
    if not wire_equal:
        raise RuntimeError("Persistent v1 wire bytes changed")
    (root / "manifest.json").write_text(json.dumps({"variants": manifest, "trace_equal": identical,
                                                  "event_trace_equal": event_equal, "wire_equal": wire_equal,
                                                  "lifecycle_coverage": lifecycle_coverage,
                                                  "event_coverage": event_coverage}, indent=2))
    if not identical:
        import difflib
        (root / "trace.diff").write_text("\n".join(difflib.unified_diff(*traces)))
        raise RuntimeError("Baseline/candidate observable lifecycle traces differ")
    if not event_equal:
        import difflib
        (root / "event-trace.diff").write_text("\n".join(difflib.unified_diff(*event_traces)))
        raise RuntimeError("Baseline/candidate observable event traces differ")


if __name__ == "__main__":
    main()
