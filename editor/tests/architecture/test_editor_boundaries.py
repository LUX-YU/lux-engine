"""Exercise the real CMake exporter and checker with isolated, dependency-free projects."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(argv):
    return subprocess.run(list(map(str, argv)), capture_output=True, text=True, encoding="utf-8", errors="replace")


def foundation_cases(args):
    # Every graph comes from the same real CMake exporter used by the engine configure.
    cases = [
        ("X01-R2-01", "edit_history", "edit_sessions", "FOUNDATION_DIRECT_DEPENDENCY", "", False),
        ("X01-R2-02", "edit_sessions", "scene_composition", "FOUNDATION_FORBIDDEN_DEPENDENCY", "", False),
        ("X01-R2-03", "edit_sessions", "ui_fixture", "FOUNDATION_FORBIDDEN_DEPENDENCY", "", False),
        ("X01-R2-04", "edit_sessions", "ui_fixture", "FOUNDATION_FORBIDDEN_DEPENDENCY", "", True),
        ("X01-R2-05", "edit_sessions", None, "FOUNDATION_FORBIDDEN_INCLUDE", "lux/engine/ui/Pane.hpp", False),
        ("legal", "edit_sessions", "edit_history", None, "", False),
        ("legal-external", "edit_sessions", "lux::cxx::container", None, "", False),
        ("imported-hop", "edit_sessions", "lux::cxx::container", "FOUNDATION_FORBIDDEN_DEPENDENCY", "", False),
        ("conditional", "edit_sessions", "ui_fixture", "FOUNDATION_FORBIDDEN_DEPENDENCY", "", False),
        ("contracts-ui", "editor_contracts", "ui_fixture", "FOUNDATION_FORBIDDEN_DEPENDENCY", "", False),
        ("history-ui", "edit_history", "ui_fixture", "FOUNDATION_FORBIDDEN_DEPENDENCY", "", False),
        ("context", "edit_sessions", "editor_context", "NEW_DEPENDS_ON_OLD_TARGET", "", False),
        ("unknown", "edit_sessions", "unresolved_library", "FOUNDATION_UNRESOLVED_DEPENDENCY", "", False),
    ]
    locations = {
        "editor_contracts": "editor/contracts", "edit_history": "editor/editing/history",
        "edit_sessions": "editor/editing/sessions", "scene_composition": "engine/scene/composition",
        "ui_fixture": "modules/function/ui", "editor_context": "editor/context",
        "identity": "modules/resource/identity",
    }
    observations = []
    for name, source, destination, expected, include, transitive in cases:
        with tempfile.TemporaryDirectory(prefix="lux-p01-r1-") as temporary:
            root = Path(temporary)
            assert run(["git", "init", root]).returncode == 0
            tools = root / "editor/tests/architecture"
            tools.mkdir(parents=True)
            for filename in ["rules.json", "check_editor_boundaries.py"]:
                shutil.copyfile(args.source / "editor/tests/architecture" / filename, tools / filename)
            for target, path in locations.items():
                directory = root / path
                directory.mkdir(parents=True, exist_ok=True)
                (directory / "CMakeLists.txt").write_text(
                    f"add_library({target} INTERFACE)\nadd_library(fixture::{target} ALIAS {target})\n",
                    encoding="utf-8")
            top = ('cmake_minimum_required(VERSION 3.22)\nproject(foundation LANGUAGES NONE)\n'
                   'set(LUX_EDITOR_MIGRATION_STAGE P01 CACHE STRING "")\n')
            top += ('add_library(stduuid INTERFACE IMPORTED)\n'
                    'add_library(lux::cxx::container INTERFACE IMPORTED)\n'
                    'add_library(lux::cxx::compile_time INTERFACE IMPORTED)\n')
            top += "".join(f"add_subdirectory({p})\n" for p in locations.values())
            if name == "legal-external":
                top += ('target_link_libraries(identity INTERFACE stduuid)\n'
                        'target_link_libraries(edit_history INTERFACE editor_contracts lux::cxx::compile_time)\n'
                        'target_link_libraries(edit_sessions INTERFACE identity edit_history editor_contracts)\n')
            if name == "imported-hop":
                top += 'set_property(TARGET lux::cxx::container PROPERTY INTERFACE_LINK_LIBRARIES fixture::ui_fixture)\n'
            top += (f'include("{args.source.as_posix()}/cmake/EditorArchitectureChecks.cmake")\n'
                    'lux_editor_check_architecture()\n')
            (root / "CMakeLists.txt").write_text(top, encoding="utf-8")
            cmake = root / locations[source] / "CMakeLists.txt"
            original = cmake.read_text()
            if destination:
                edge = f"fixture::{destination}" if destination in locations else destination
                if name == "conditional":
                    edge = f"$<$<CONFIG:Debug>:{edge}>"
                if transitive:
                    text = (f"add_library(hop INTERFACE)\n"
                            f"target_link_libraries({source} INTERFACE hop)\n"
                            f'target_link_libraries(hop INTERFACE "$<LINK_ONLY:{edge}>")\n')
                else:
                    text = f'target_link_libraries({source} INTERFACE "$<LINK_ONLY:{edge}>")\n'
                cmake.write_text(original + text, encoding="utf-8")
            header = root / locations[source] / "probe.hpp"
            header.write_text(f"#include <{include}>\n" if include else "", encoding="utf-8")
            build = root / "build"
            result = run([args.cmake, "-S", root, "-B", build])
            text = result.stdout + result.stderr
            rejected = result.returncode != 0 and expected in text if expected else result.returncode == 0
            if name == "X01-R2-04":
                rejected = rejected and "edit_sessions -> hop -> ui_fixture" in " ".join(text.split())
            if name == "imported-hop":
                rejected = rejected and "edit_sessions -> lux::cxx::container -> ui_fixture" in " ".join(text.split())
            graph = json.loads((build / "editor-architecture/targets.json").read_text())
            cmake.write_text(original, encoding="utf-8")
            header.write_text("", encoding="utf-8")
            if name == "imported-hop":
                (root / "CMakeLists.txt").write_text(top.replace(
                    "PROPERTY INTERFACE_LINK_LIBRARIES fixture::ui_fixture", 'PROPERTY INTERFACE_LINK_LIBRARIES ""'),
                    encoding="utf-8")
            repaired = run([args.cmake, "-S", root, "-B", build])
            passed = rejected and repaired.returncode == 0
            observations.append({"id": name, "expected_rule": expected, "exit_code": result.returncode,
                                 "matched_expected_rule": rejected, "repaired_exit_code": repaired.returncode,
                                 "passed": passed, "graph": graph,
                                 "log": text, "repaired_log": repaired.stdout + repaired.stderr})
            print(f"{'PASS' if passed else 'FAIL'} {name}: configure={result.returncode} "
                  f"expected={expected} repaired={repaired.returncode}", flush=True)
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps(observations, indent=2) + "\n", encoding="utf-8")
    return 0 if all(x["passed"] for x in observations) else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--foundation-only", action="store_true")
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    if args.foundation_only:
        return foundation_cases(args)
    with tempfile.TemporaryDirectory(prefix="lux-p00-") as directory:
        root = Path(directory)
        assert run(["git", "init", root]).returncode == 0
        tools = root / "editor/tests/architecture"
        tools.mkdir(parents=True)
        for name in ("rules.json", "check_editor_boundaries.py"):
            shutil.copyfile(args.source / "editor/tests/architecture" / name, tools / name)
        cases = [
            ("engine/probe", "engine_probe", "editor/bootstrap", "editor_bootstrap",
             "ENGINE_DEPENDS_ON_EDITOR", ""),
            ("editor/workflows", "editor_workflows", "editor/bootstrap", "editor_bootstrap",
             "WORKFLOWS_DEPEND_ON_BOOTSTRAP", ""),
            ("editor/tools/scene/model", "scene_model", "modules/function/ui", "ui_fixture",
             "MODEL_DEPENDS_ON_UI", '#include <lux/engine/ui/Pane.hpp>\n'),
        ]
        for index, (src, target, dest, dependency, rule, header) in enumerate(cases):
            for path in (src, dest):
                (root / path).mkdir(parents=True, exist_ok=True)
            (root / dest / "CMakeLists.txt").write_text(
                f"add_library({dependency} INTERFACE)\n"
                f"add_library(fixture::destination ALIAS {dependency})\n", encoding="utf-8")
            (root / src / "probe.hpp").write_text(header, encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.22)\nproject(fixture LANGUAGES NONE)\n'
                f'add_subdirectory({dest})\nadd_subdirectory({src})\n'
                f'include("{args.source.as_posix()}/cmake/EditorArchitectureChecks.cmake")\n'
                'lux_editor_check_architecture()\n', encoding="utf-8")
            cmake = root / src / "CMakeLists.txt"
            # The wrong edge is transitive and behind LINK_ONLY, using a real CMake alias.
            cmake.write_text(f'add_library({target} INTERFACE)\nadd_library(hop INTERFACE)\n'
                             f'target_link_libraries({target} INTERFACE hop)\n'
                             'target_link_libraries(hop INTERFACE "$<LINK_ONLY:fixture::destination>")\n',
                             encoding="utf-8")
            build = root / f"build-{index}"
            result = run([args.cmake, "-S", root, "-B", build])
            assert result.returncode != 0 and rule in result.stdout + result.stderr, result.stdout + result.stderr
            cmake.write_text(f"add_library({target} INTERFACE)\n", encoding="utf-8")
            (root / src / "probe.hpp").write_text("", encoding="utf-8")
            result = run([args.cmake, "-S", root, "-B", build])
            assert result.returncode == 0, result.stdout + result.stderr
            print(f"PASS {rule}: forbidden edge rejected; same environment repaired")

        # Expired paths and unregistered transitions are independent of active target_sources.
        from check_editor_boundaries import inspect
        rules = json.loads((tools / "rules.json").read_text(encoding="utf-8"))
        old = root / rules["expired_paths"][0]["path"]
        old.parent.mkdir(parents=True, exist_ok=True)
        old.write_text("// retired\n", encoding="utf-8")
        assert any(x["rule"] == "EXPIRED_PATH" for x in inspect(root, [], rules, "P01"))
        assert not any(x["rule"] == "EXPIRED_PATH" for x in inspect(root, [], rules, "P00"))
        old.unlink()
        private = root / "editor/tools/scene/model/Private.hpp"
        private.write_text('#include "../../pinclude/Private.hpp"\n', encoding="utf-8")
        assert any(x["rule"] == "PRIVATE_INCLUDE" for x in inspect(root, [], rules, "P00"))
        private.unlink()
        print("PASS expiry and private header checks")

        history = root / "editor/editing/history/probe.hpp"
        history.parent.mkdir(parents=True, exist_ok=True)
        history.write_text("struct History { void beginSave(); };\n", encoding="utf-8")
        assert any(x["rule"] == "HISTORY_PERSISTENCE_API" for x in inspect(root, [], rules, "P01"))
        history.write_text("struct History { bool clean; };\n", encoding="utf-8")
        assert any(x["rule"] == "HISTORY_PERSISTENCE_STATE" for x in inspect(root, [], rules, "P01"))
        history.write_text('#include "LegacyPersistenceState.hpp"\n', encoding="utf-8")
        assert any(x["rule"] == "NEW_DEPENDS_ON_TRANSITION" for x in inspect(root, [], rules, "P01"))
        history.write_text("struct History {};\n", encoding="utf-8")
        assert not inspect(root, [], rules, "P01")
        print("PASS P01 retired history API, saved state and new-to-transition checks")
        for name in ["history", "sessions"]:
            retired_directory = root / "editor" / name
            retired_directory.mkdir()
            assert any(x["rule"] == "EXPIRED_PATH" and x["path"] == f"editor/{name}"
                       for x in inspect(root, [], rules, "P02"))
            retired_directory.rmdir()
        assert not inspect(root, [], rules, "P02")
        print("PASS S01 old directories rejected; narrow history/session scopes preserved")


        # Test options must be subordinate to BUILD_TESTING and default to native-only.
        (root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\nproject(options LANGUAGES NONE)\n'
            'set(BUILD_TESTING ON)\n'
            f'include("{args.source.as_posix()}/cmake/EditorTests.cmake")\n'
            'if(NOT LUX_EDITOR_BUILD_NATIVE_TESTS OR LUX_EDITOR_TEST_DESKTOP_GPU OR '
            'LUX_EDITOR_BUILD_TOOLCHAIN_TESTS OR LUX_EDITOR_BUILD_INSTALLED_TESTS)\n'
            'message(FATAL_ERROR "Incorrect native-only defaults")\nendif()\n', encoding="utf-8")
        result = run([args.cmake, "-S", root, "-B", root / "options"])
        assert result.returncode == 0, result.stdout + result.stderr
        print("PASS native-only option defaults")
    return foundation_cases(args)


if __name__ == "__main__":
    raise SystemExit(main())
