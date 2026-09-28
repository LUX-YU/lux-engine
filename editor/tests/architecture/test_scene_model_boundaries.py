"""P02 boundaries: real CMake alias/LINK_ONLY/imported graphs and source includes, then repair."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(args):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, encoding="utf-8", errors="replace")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    locations = {
        "scene_model": "editor/tools/scene/model", "scene_asset": "engine/scene/asset",
        "scene_composition": "engine/scene/composition", "ui_fixture": "modules/function/ui",
        "editor_context": "editor/context",
    }
    cases = [
        ("direct-runtime", "scene_composition", "", False, False),
        ("intermediate-ui", "ui_fixture", "", True, False),
        ("intermediate-context", "editor_context", "", True, False),
        ("imported-ui", "ui_fixture", "", True, True),
        ("old-editor-header", None, "lux/engine/editor/scene/SceneEditor.hpp", False, False),
        ("old-context-header", None, "lux/engine/editor/EditorContext.hpp", False, False),
        ("old-bridge-header", None, "LegacyPersistenceState.hpp", False, False),
        ("runtime-header", None, "lux/engine/scene/SceneRuntime.hpp", False, False),
        ("unknown-library", "unresolved_model_library", "", False, False),
        ("legal-cpu", None, "lux/engine/scene/ScenePackage.hpp", False, False),
    ]
    evidence = []
    for name, destination, header, transitive, imported in cases:
        with tempfile.TemporaryDirectory(prefix="lux-p02-boundary-") as directory:
            root = Path(directory)
            assert run(["git", "init", root]).returncode == 0
            policy = root / "editor/tests/architecture"
            policy.mkdir(parents=True)
            for file in ["rules.json", "check_editor_boundaries.py"]:
                shutil.copyfile(args.source / "editor/tests/architecture" / file, policy / file)
            for target, path in locations.items():
                folder = root / path
                folder.mkdir(parents=True, exist_ok=True)
                (folder / "CMakeLists.txt").write_text(
                    f"add_library({target} INTERFACE)\nadd_library(fixture::{target} ALIAS {target})\n")
            top = ('cmake_minimum_required(VERSION 3.22)\nproject(scene_model_boundary LANGUAGES NONE)\n'
                   'set(LUX_EDITOR_MIGRATION_STAGE P02 CACHE STRING "")\n')
            top += "".join(f"add_subdirectory({path})\n" for path in locations.values())
            edge = ""
            if destination:
                target = f"fixture::{destination}" if destination in locations else destination
                if transitive:
                    edge = 'target_link_libraries(scene_model INTERFACE fixture::scene_asset)\n'
                    if imported:
                        edge += ('add_library(lux::cxx::memory INTERFACE IMPORTED)\n'
                                 'target_link_libraries(scene_asset INTERFACE lux::cxx::memory)\n'
                                 f'set_property(TARGET lux::cxx::memory PROPERTY INTERFACE_LINK_LIBRARIES '
                                 f'"$<LINK_ONLY:{target}>")\n')
                    else:
                        edge += f'target_link_libraries(scene_asset INTERFACE "$<LINK_ONLY:{target}>")\n'
                else:
                    edge = f'target_link_libraries(scene_model INTERFACE "$<LINK_ONLY:{target}>")\n'
            tail = (f'include("{args.source.as_posix()}/cmake/EditorArchitectureChecks.cmake")\n'
                    'lux_editor_check_architecture()\n')
            (root / "CMakeLists.txt").write_text(top + edge + tail)
            probe = root / locations["scene_model"] / "probe.hpp"
            probe.write_text(f"#include <{header}>\n" if header else "")
            build = root / "build"
            result = run([args.cmake, "-S", root, "-B", build])
            text = result.stdout + result.stderr
            flat = " ".join(text.split())
            if name == "legal-cpu":
                rule = None
                rejected = result.returncode == 0
            else:
                rule = ("SCENE_MODEL_FORBIDDEN_INCLUDE" if header else
                        "SCENE_MODEL_UNRESOLVED_DEPENDENCY" if name == "unknown-library" else
                        "SCENE_MODEL_FORBIDDEN_DEPENDENCY")
                chain = "scene_model -> "
                if transitive:
                    chain += "scene_asset -> "
                if imported:
                    chain += "lux::cxx::memory -> "
                chain += destination or ""
                rejected = result.returncode != 0 and rule in text and (bool(header) or chain in flat)
            graph_path = build / "editor-architecture/targets.json"
            graph = json.loads(graph_path.read_text()) if graph_path.exists() else None
            (root / "CMakeLists.txt").write_text(top + tail)
            probe.write_text("")
            repaired = run([args.cmake, "-S", root, "-B", build])
            passed = rejected and graph is not None and repaired.returncode == 0
            evidence.append({"id": name, "expected_rule": rule, "exit_code": result.returncode,
                "passed": passed, "graph": graph, "log": text, "repaired_exit_code": repaired.returncode,
                "repaired_log": repaired.stdout + repaired.stderr})
            print(f"{'PASS' if passed else 'FAIL'} {name}: {result.returncode} -> {repaired.returncode}", flush=True)
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps(evidence, indent=2) + "\n")
    return 0 if all(item["passed"] for item in evidence) else 1


if __name__ == "__main__":
    raise SystemExit(main())
