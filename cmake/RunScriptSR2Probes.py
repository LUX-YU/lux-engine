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


def run(command, log, env=None):
    with log.open("w", encoding="utf-8") as stream:
        stream.write(json.dumps(command) + "\n")
        stream.flush()
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, env=env)
    if result.returncode:
        raise RuntimeError(f"{log}: exit {result.returncode}")


def instrument(source, candidate):
    source = source.replace('"../../../system/test/HookInvocationTestAccess.hpp"',
                            '"HookInvocationTestAccess.hpp"')
    source = '#include <cstdio>\n#include <chrono>\n#include <string_view>\n' + source
    source = source.replace("namespace\n{", """namespace
{
    bool trace_enabled{};
    void trace(const char* event, std::size_t instance, std::uint64_t symbol = 0U) noexcept
    {
        if (trace_enabled)
            std::printf("%s,%zu,%llu\\n", event, instance, static_cast<unsigned long long>(symbol));
    }
""", 1)
    if not candidate:
        source = source.replace("        std::vector<ScriptAwaitableCompletion> completions;",
                                "        std::vector<ScriptAwaitableCompletion> completions;\n"
                                "        std::vector<ScriptBehavior*> hosts;")
        source = source.replace("        const ScriptInstanceCreateContext&,",
                                "        const ScriptInstanceCreateContext& create_context,")
        source = source.replace("        ++state.creates;\n        output.value = instance;",
                                "        ++state.creates;\n        state.hosts.push_back(create_context.behavior);\n"
                                "        output.value = instance;")
    source = source.replace("        ++state.creates;", '        ++state.creates;\n        trace("create", state.creates);')
    source = source.replace("        ++state.prepares;", '        ++state.prepares;\n'
                            '        trace("prepare", static_cast<Instance*>(instance.value)->serial, function.symbol_id);')
    source = source.replace("        ++static_cast<BackendState*>(context)->releases;",
                            '        trace("release", static_cast<PreparedCall*>(method.token)->instance->serial,\n'
                            '            static_cast<PreparedCall*>(method.token)->symbol);\n'
                            '        ++static_cast<BackendState*>(context)->releases;')
    source = source.replace("        ++static_cast<BackendState*>(context)->destroys;",
                            '        trace("destroy", static_cast<Instance*>(instance.value)->serial);\n'
                            '        ++static_cast<BackendState*>(context)->destroys;')
    source = source.replace("        auto& state = *instance.owner;",
                            '        auto& state = *instance.owner;\n        trace("invoke", instance.serial, call.symbol);')
    source = source.replace("        ++continuation->owner->continuation_destroys;",
                            '        trace("continuation-destroy", continuation->instance->serial);\n'
                            '        ++continuation->owner->continuation_destroys;')
    source = source[:source.index("int main()")]
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
    cross_batch = cross_batch.replace("INITIAL_CROSS_BATCH", """
    std::swap(harness.description[1], harness.description[2]);
    auto created = harness.create(2U);
    std::swap(harness.description[1], harness.description[2]);
""" if candidate else "auto created = harness.create();")
    cross_batch = cross_batch.replace("SUBMIT_SECOND", "harness.submitEntity(system, 1U);" if candidate else "")
    cross_batch = cross_batch.replace("SUBMIT_THIRD", "harness.submitEntity(system, 2U);" if candidate else "")
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
    const auto start = Clock::now();
    auto created = harness.create(INITIAL_COUNT);
    assert(created && created->prepare());
    auto& system = *created;
    const auto prepared = Clock::now();
    for (std::size_t index{1U}; index < 8U; ++index)
        harness.entities[index] = harness.registry.create();
    const auto late_start = Clock::now();
    SUBMIT_LATE
    assert(system.processLifecycle());
    const auto late_end = Clock::now();
    assert(system.activeInstanceCount() == 8U);
    std::int64_t remount_ns{};
    for (std::size_t iteration{}; iteration < 144U; ++iteration)
    {
        const auto before = Clock::now();
        harness.registry.destroy(harness.entities[0]);
        harness.entities[0] = harness.registry.create();
        SUBMIT_REPLACEMENT
        assert(system.processLifecycle());
        const auto after = Clock::now();
        if (iteration >= 16U)
            remount_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
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
    return 0;
}
"""
    source = source.replace("INITIAL_COUNT", "1U" if candidate else "")
    source = source.replace("SUBMIT_LATE", """
    std::array<ScriptMountStatus, 8U> feedback;
    assert(system.collectMountStatusChanges(feedback));
    for (std::size_t index{1U}; index < 8U; ++index)
        harness.description[index].scope = EntityScriptScope{harness.entities[index]};
    assert(system.mountResolvedBatch(std::span{harness.description}.subspan(1U)));
""" if candidate else "")
    source = source.replace("SUBMIT_REPLACEMENT", "harness.submitEntity(system, 0U);" if candidate else "")
    return source


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
        probe = instrument(original.read_text(encoding="utf-8-sig"), variant == "candidate")
        (directory / "probe.cpp").write_text(probe, encoding="utf-8")
        helper = source_root / "engine/domain/simulation/system/test/HookInvocationTestAccess.hpp"
        (directory / helper.name).write_bytes(helper.read_bytes())
        wire_path = source_root / ("engine/scene/integration/script_description/test/script_system_description_test.cpp"
                                   if variant == "candidate" else
                                   "engine/domain/simulation/builtin/script/test/script_system_description_test.cpp")
        golden_header = wire_path.parent / "ScriptSystemV1Golden.hpp"
        if golden_header.exists():
            (directory / golden_header.name).write_bytes(golden_header.read_bytes())
        wire = '#include <fstream>\n' + wire_path.read_text(encoding="utf-8-sig")
        wire = wire.replace('int main()', 'int main(int argc, char** argv)')
        wire = wire.replace('    assert(encoded);', '    assert(encoded);\n'
                            '    assert(argc == 2);\n'
                            '    std::ofstream golden(argv[1], std::ios::binary);\n'
                            '    golden.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());\n'
                            '    assert(golden.good());\n')
        (directory / "wire.cpp").write_text(wire, encoding="utf-8")
        (directory / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.22)
project(sr2_probe LANGUAGES CXX)
find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)
add_executable(sr2_probe probe.cpp)
target_compile_features(sr2_probe PRIVATE cxx_std_20)
target_compile_options(sr2_probe PRIVATE /UNDEBUG)
target_link_libraries(sr2_probe PRIVATE lux::engine::simulation::simulation_script)
''', encoding="utf-8")
        with (directory / "CMakeLists.txt").open("a", encoding="utf-8") as cmake:
            cmake.write("\nadd_executable(sr2_wire wire.cpp)\n"
                        "target_compile_features(sr2_wire PRIVATE cxx_std_20)\n"
                        "target_compile_options(sr2_wire PRIVATE /UNDEBUG)\n")
            if variant == "candidate":
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
    traces = [(root / variant / "trace.log").read_text().splitlines()[1:] for variant in variants]
    identical = traces[0] == traces[1]
    wire_equal = (root / "baseline/v1.bin").read_bytes() == (root / "candidate/v1.bin").read_bytes()
    if not wire_equal:
        raise RuntimeError("Persistent v1 wire bytes changed")
    (root / "manifest.json").write_text(json.dumps({"variants": manifest, "trace_equal": identical, "wire_equal": wire_equal}, indent=2))
    if not identical:
        import difflib
        (root / "trace.diff").write_text("\n".join(difflib.unified_diff(*traces)))
        raise RuntimeError("Baseline/candidate observable lifecycle traces differ")


if __name__ == "__main__":
    main()
