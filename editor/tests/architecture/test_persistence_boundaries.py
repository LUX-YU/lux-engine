"""P05: configure real target graphs, reject forbidden edges/includes, then repair each fixture."""
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
    locations.update(editor_context="editor/context", ui_fixture="modules/function/ui", process_execution="engine/process/execution")
    cases = [
        ("core-model", "editor_persistence", None, "material_model", ""),
        ("core-transitive-ui", "editor_persistence", "edit_sessions", "ui_fixture", ""),
        ("core-context-header", "editor_persistence", None, None, "lux/engine/editor/EditorContext.hpp"),
        ("io-transitive-context", "project_io", "process_execution", "editor_context", ""),
        ("io-old-storage", "project_io", None, None, "lux/engine/editor/ProjectStorage.hpp"),
    ]
    for model in ["scene", "material", "flowforge"]:
        cases.extend([
            (model + "-reverse", model + "_model", None, "editor_persistence", ""),
            (model + "-adapter-ui", model + "_persistence", model + "_model", "ui_fixture", ""),
            (model + "-adapter-bridge", model + "_persistence", None, None, "LegacyPersistenceState.hpp"),
            (model + "-legal", model + "_persistence", model + "_model", None, ""),
        ])
    evidence = []
    for name, target, intermediate, forbidden, header in cases:
        with tempfile.TemporaryDirectory(prefix="lux-p05-boundary-") as directory:
            root = Path(directory)
            assert run(["git", "init", root]).returncode == 0
            policy = root / "editor/tests/architecture"
            policy.mkdir(parents=True)
            for file in ["rules.json", "check_editor_boundaries.py"]:
                shutil.copyfile(repo / "editor/tests/architecture" / file, policy / file)
            targets = list(dict.fromkeys(x for x in [target, intermediate, forbidden] if x))
            top = "cmake_minimum_required(VERSION 3.22)\nproject(persistence_boundary LANGUAGES CXX)\nset(LUX_EDITOR_MIGRATION_STAGE P05)\n"
            for value in targets:
                folder = root / locations[value]
                folder.mkdir(parents=True, exist_ok=True)
                (folder / "dummy.cpp").write_text("int " + value + "_fixture;\n")
                (folder / "CMakeLists.txt").write_text(f"add_library({value} STATIC dummy.cpp)\nadd_library(fixture::{value} ALIAS {value})\n")
                top += f"add_subdirectory({locations[value]})\n"
            edges = ""
            if intermediate:
                edges += f"target_link_libraries({target} PRIVATE fixture::{intermediate})\n"
            if forbidden:
                edges += f'target_link_libraries({intermediate or target} PRIVATE "$<LINK_ONLY:fixture::{forbidden}>")\n'
            tail = f'include("{repo.as_posix()}/cmake/EditorArchitectureChecks.cmake")\nlux_editor_check_architecture()\n'
            (root / "CMakeLists.txt").write_text(top + edges + tail)
            probe = root / locations[target] / "probe.hpp"
            probe.write_text(f"#include <{header}>\n" if header else "")
            build = root / "build"
            result = run([args.cmake, "-S", root, "-B", build, "-G", "Ninja"])
            output = result.stdout + result.stderr
            rule = target.upper() + ("_FORBIDDEN_INCLUDE" if header else "_FORBIDDEN_DEPENDENCY")
            legal = name.endswith("-legal")
            rejected = result.returncode == 0 if legal else result.returncode != 0 and rule in output
            if forbidden:
                chain = " -> ".join(x for x in [target, intermediate, forbidden] if x)
                rejected = rejected and chain in " ".join(output.split())
            graph = json.loads((build / "editor-architecture/targets.json").read_text())
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
