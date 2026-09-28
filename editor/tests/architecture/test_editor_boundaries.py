"""Exercise the real CMake exporter and checker with isolated, dependency-free projects."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(argv):
    return subprocess.run(list(map(str, argv)), capture_output=True, text=True, encoding="utf-8", errors="replace")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
    args = parser.parse_args()
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
