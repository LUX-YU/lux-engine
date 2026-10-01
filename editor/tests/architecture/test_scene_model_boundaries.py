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
    parser.add_argument("--model", choices=["scene", "material", "flowforge"], default="scene")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    args.source = args.source.resolve()
    locations = {
        "scene_model": "editor/authoring/scene", "scene_asset": "engine/scene/asset",
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
    model = args.model + "_model"
    stage = {"scene": "P02", "material": "P03", "flowforge": "P04"}[args.model]
    pure = "material_graph" if args.model == "material" else "scene_asset"
    if args.model == "material":
        locations.pop("scene_model"); locations.pop("scene_asset")
        locations.update({model: "editor/authoring/material", pure: "modules/function/material",
            "compiler_fixture": "engine/toolchain/material_compiler", "storage_fixture": "editor/storage"})
        cases = [(name, dest, "lux/engine/material/graph/MaterialSource.hpp" if name == "legal-cpu" else header,
            transitive, imported) for name, dest, header, transitive, imported in cases]
        cases.extend([("compiler", "compiler_fixture", "", True, False),
            ("storage", "storage_fixture", "", True, False),
            ("old-material-header", None, "lux/engine/editor/material/MaterialEditor.hpp", False, False),
            ("preview-header", None, "lux/engine/editor/material/MaterialPreview.hpp", False, False),
            ("static-private-runtime", "scene_composition", "", True, False),
            ("static-private-imported", "ui_fixture", "", True, True)])
    if args.model == "flowforge":
        pure = "flowforge"
        locations.pop("scene_model"); locations.pop("scene_asset")
        locations.update({model: "editor/authoring/flow", pure: "modules/function/flowforge",
            "compiler_fixture": "engine/toolchain/flowforge", "storage_fixture": "editor/storage",
            "script_runtime_fixture": "engine/domain/script"})
        cases = [(name, dest, "lux/engine/flowforge/graph/FlowSource.hpp" if name == "legal-cpu" else header,
            transitive, imported) for name, dest, header, transitive, imported in cases]
        cases.extend([("direct-compiler", "compiler_fixture", "", False, False),
            ("compiler", "compiler_fixture", "", True, False),
            ("script-execution", "script_runtime_fixture", "", True, False),
            ("storage", "storage_fixture", "", True, False),
            ("old-flow-header", None, "lux/engine/editor/flowforge/FlowForgeEditor.hpp", False, False),
            ("compiler-header", None, "lux/engine/flowforge/Compiler.hpp", False, False),
            ("static-private-runtime", "scene_composition", "", True, False),
            ("static-private-imported", "ui_fixture", "", True, True)])
    evidence = []
    for name, destination, header, transitive, imported in cases:
        with tempfile.TemporaryDirectory(prefix="lux-" + model + "-boundary-") as directory:
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
            if name.startswith("static-private"):
                top = top.replace("LANGUAGES NONE", "LANGUAGES CXX")
                for target_name in [model, pure]:
                    folder = root / locations[target_name]
                    (folder / "dummy.cpp").write_text("int " + target_name + "_fixture;\n")
                    (folder / "CMakeLists.txt").write_text(
                        f"add_library({target_name} STATIC dummy.cpp)\n"
                        f"add_library(fixture::{target_name} ALIAS {target_name})\n")
                edge = edge.replace("scene_model INTERFACE", "scene_model PRIVATE")
                edge = edge.replace("scene_asset INTERFACE", "scene_asset PRIVATE")
            top = top.replace("P02", stage)
            edge = edge.replace("scene_model", model).replace("scene_asset", pure)
            (root / "CMakeLists.txt").write_text(top + edge + tail)
            probe = root / locations[model] / "probe.hpp"
            probe.write_text(f"#include <{header}>\n" if header else "")
            provider = root / locations[model] / "CMakeLists.txt"
            provider.write_text(provider.read_text() + f'target_sources({model} INTERFACE "${{CMAKE_CURRENT_SOURCE_DIR}}/probe.hpp")\n')
            build = root / "build"
            result = run([args.cmake, "-S", root, "-B", build])
            text = result.stdout + result.stderr
            flat = " ".join(text.split())
            if name == "legal-cpu":
                rule = None
                rejected = result.returncode == 0
            else:
                rule = (model.upper() + "_FORBIDDEN_INCLUDE" if header else
                        model.upper() + "_UNRESOLVED_DEPENDENCY" if name == "unknown-library" else
                        model.upper() + "_FORBIDDEN_DEPENDENCY")
                chain = model + " -> "
                if transitive:
                    chain += pure + " -> "
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
