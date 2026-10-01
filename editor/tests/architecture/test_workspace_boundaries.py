"""P09: actual CMake workspace dependency and include negative fixtures."""
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
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--evidence", required=True, type=Path)
    args = parser.parse_args()
    repo = args.source.resolve()
    rules = json.loads((repo / "editor/tests/architecture/rules.json").read_text())
    locations = {x["name"]: x["path"] for x in rules["targets"]}
    locations.update(editor_storage="editor/storage", editor_context="editor/context", ui_fixture="modules/function/ui", process_execution="engine/process/execution")
    locations.update(scene_composition="engine/scene/composition", render_runtime="modules/function/render/runtime")
    cases = [
        ("layout-scene", "layout_model", None, "scene_model", ""),
        ("layout-material-transitive", "layout_model", "editor_contracts", "material_model", ""),
        ("layout-runtime", "layout_model", None, "scene_composition", ""),
        ("layout-ui", "layout_model", None, "ui_fixture", ""),
        ("layout-io", "layout_model", None, "editor_storage", ""),
        ("recovery-flow", "recovery_model", None, "flowforge_model", ""),
        ("recovery-transitive-context", "recovery_model", "editor_contracts", "editor_context", ""),
        ("store-context", "workspace_store", None, "editor_context", ""),
        ("store-transitive-context", "workspace_store", "layout_model", "editor_context", ""),
        ("engine-editor", "ui_fixture", None, "workspace_store", ""),
        ("layout-root", "layout_model", None, None, "lux/engine/ui/Root.hpp"),
        ("layout-pane", "layout_model", None, None, "lux/engine/ui/Pane.hpp"),
        ("layout-store-header", "layout_model", None, None, "lux/engine/editor/workspace/WorkspaceStore.hpp"),
        ("layout-storage-header", "layout_model", None, None, "lux/engine/editor/storage/FileArtifactStore.hpp"),
        ("store-old-workspace", "workspace_store", None, None, "lux/engine/editor/WorkspaceRequest.hpp"),
        ("layout-legal", "layout_model", "editor_contracts", None, ""),
        ("recovery-legal", "recovery_model", "editor_contracts", None, ""),
        ("store-legal", "workspace_store", "layout_model", None, ""),
    ]
    evidence = []
    for name, target, intermediate, forbidden, header in cases:
        with tempfile.TemporaryDirectory(prefix="lux-p09-boundary-") as directory:
            root = Path(directory)
            assert run(["git", "init", root]).returncode == 0
            policy = root / "editor/tests/architecture"
            policy.mkdir(parents=True)
            for file in ["rules.json", "check_editor_boundaries.py"]:
                shutil.copyfile(repo / "editor/tests/architecture" / file, policy / file)
            targets = list(dict.fromkeys(x for x in [target, intermediate, forbidden] if x))
            top = "cmake_minimum_required(VERSION 3.22)\nproject(execution_boundary LANGUAGES CXX)\nset(LUX_EDITOR_MIGRATION_STAGE P09)\n"
            for value in targets:
                folder = root / locations[value]
                folder.mkdir(parents=True, exist_ok=True)
                (folder / (value + ".cpp")).write_text("int " + value + "_fixture;\n")
                cmake_file = folder / "CMakeLists.txt"
                previous = cmake_file.read_text() if cmake_file.exists() else ""
                cmake_file.write_text(previous + f"add_library({value} STATIC {value}.cpp)\nadd_library(fixture::{value} ALIAS {value})\n")
                entry = f"add_subdirectory({locations[value]})\n"
                if entry not in top:
                    top += entry
            edges = ""
            if intermediate:
                edges += f"target_link_libraries({target} PRIVATE fixture::{intermediate})\n"
            if forbidden:
                edges += f'target_link_libraries({intermediate or target} PRIVATE "$<LINK_ONLY:fixture::{forbidden}>")\n'
            tail = f'include("{repo.as_posix()}/cmake/EditorArchitectureChecks.cmake")\nlux_editor_check_architecture()\n'
            (root / "CMakeLists.txt").write_text(top + edges + tail)
            probe = root / locations[target] / "probe.hpp"
            probe.write_text(f"#include <{header}>\n" if header else "")
            provider = root / locations[target] / "CMakeLists.txt"
            provider.write_text(provider.read_text() + f'target_sources({target} PRIVATE "{probe.as_posix()}")\n')
            build = root / "build"
            result = run([args.cmake, "-S", root, "-B", build, "-G", "Ninja"])
            output = result.stdout + result.stderr
            rule = target.upper() + ("_FORBIDDEN_INCLUDE" if header else "_FORBIDDEN_DEPENDENCY")
            if target == "ui_fixture": rule = "ENGINE_DEPENDS_ON_EDITOR"
            legal = name.endswith("-legal")
            rejected = result.returncode == 0 if legal else result.returncode != 0 and rule in output
            if forbidden:
                chain = " -> ".join(x for x in [target, intermediate, forbidden] if x)
                rejected = rejected and chain in " ".join(output.split())
            graph_file = build / "editor-architecture/targets.json"
            graph = json.loads(graph_file.read_text()) if graph_file.exists() else []
            rejected = rejected and graph_file.exists()
            (root / "CMakeLists.txt").write_text(top + tail)
            probe.write_text("")
            repaired = run([args.cmake, "-S", root, "-B", build])
            passed = rejected and repaired.returncode == 0
            evidence.append(dict(id=name, passed=passed, expected_rule=None if legal else rule,
                exit_code=result.returncode, log=output, graph=graph, repaired_exit_code=repaired.returncode,
                repaired_log=repaired.stdout + repaired.stderr))
            print("PASS" if passed else "FAIL", name, flush=True)
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    args.evidence.write_text(json.dumps(evidence, indent=2) + "\n")
    return 0 if all(x["passed"] for x in evidence) else 1


if __name__ == "__main__":
    raise SystemExit(main())
