"""P08: actual CMake runtime/run/model dependency and private bridge negative fixtures."""
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
    parser.add_argument("--stage", choices=["P08", "P10"], default="P08")
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
        ("scene-author-interaction", "scene_model", None, "scene_interaction", ""),
        ("material-author-interaction", "material_model", None, "material_interaction", ""),
        ("flow-author-interaction", "flowforge_model", None, "flowforge_interaction", ""),
        ("scene-context", "scene_interaction", None, "editor_context", ""),
        ("material-transitive-context", "material_interaction", "material_model", "editor_context", ""),
        ("flow-transitive-ui", "flowforge_interaction", "flowforge_model", "ui_fixture", ""),
        ("scene-ui", "scene_interaction", None, "ui_fixture", ""),
        ("view-scene", "view_api", None, "scene_model", ""),
        ("view-storage", "view_api", None, "editor_storage", ""),
        ("view-render", "view_api", None, "render_runtime", ""),
        ("view-transitive-model", "view_api", "editor_contracts", "material_model", ""),
        ("engine-ui-editor", "ui_fixture", None, "view_api", ""),
        ("scene-old-bridge", "scene_interaction", None, None, "SceneRunCaptureAccess.hpp"),
        ("view-model-include", "view_api", None, None, "lux/engine/editor/scene/SceneSession.hpp"),
        ("view-rooted-factory", "view_api", None, None, "ROOTED_FACTORY"),
        ("scene-legal", "scene_interaction", "scene_model", None, ""),
        ("material-legal", "material_interaction", "material_model", None, ""),
        ("flow-legal", "flowforge_interaction", "flowforge_model", None, ""),
        ("view-legal", "view_api", "editor_contracts", None, ""),
    ]
    if args.stage == "P10":
        cases = [
            ("host-scene-owner", "view_host", None, "scene_model", ""),
            ("host-run-owner", "view_host", None, "scene_execution", ""),
            ("host-old-editor", "view_host", None, "editor_context", ""),
            ("widgets-runtime", "editor_widgets", None, "scene_composition", ""),
            ("widgets-model", "editor_widgets", None, "material_model", ""),
            ("scene-old-ui", "scene_ui", None, "editor_ui", ""),
            ("material-transitive-old", "material_ui", "scene_ui", "editor_context", ""),
            ("flow-old-editor", "flow_ui", None, "editor_flowforge", ""),
            ("harness-transitive-old", "editor_scene_views_test", "material_ui", "editor_context", ""),
            ("harness-imported-old", "editor_scene_views_test", "p10_imported_bridge", "editor_context", ""),
            ("engine-ui-new-editor", "ui_fixture", None, "scene_ui", ""),
            ("model-new-ui", "scene_model", None, "scene_ui", ""),
            ("view-api-desktop", "view_api", None, "desktop_shell", ""),
            ("project-storage", "project_ui", None, "editor_storage", ""),
            ("host-model-header", "view_host", None, None, "lux/engine/editor/scene/SceneSession.hpp"),
            ("scene-old-header", "scene_ui", None, None, "lux/engine/editor/scene/SceneEditor.hpp"),
            ("widgets-private-header", "editor_widgets", None, None, "../../tools/scene/pinclude/SceneEditorData.hpp"),
            ("scene-rooted-factory", "scene_ui", None, None, "ROOTED_FACTORY"),
            ("host-legal", "view_host", "view_api", None, ""),
            ("desktop-legal", "desktop_shell", "view_host", None, ""),
            ("widgets-legal", "editor_widgets", None, None, ""),
            ("scene-legal", "scene_ui", "scene_interaction", None, ""),
            ("material-legal", "material_ui", "material_interaction", None, ""),
            ("flow-legal", "flow_ui", "flowforge_interaction", None, ""),
            ("harness-legal", "editor_scene_views_test", "scene_ui", None, ""),
        ]
        locations.update(editor_ui="editor/ui", editor_flowforge="editor/tools/flowforge",
                         scene_execution="editor/tools/scene/execution", p10_imported_bridge="external/p10")
    evidence = []
    for name, target, intermediate, forbidden, header in cases:
        with tempfile.TemporaryDirectory(prefix="lux-p08-boundary-") as directory:
            root = Path(directory)
            assert run(["git", "init", root]).returncode == 0
            policy = root / "editor/tests/architecture"
            policy.mkdir(parents=True)
            for file in ["rules.json", "check_editor_boundaries.py"]:
                shutil.copyfile(repo / "editor/tests/architecture" / file, policy / file)
            if name == "harness-imported-old":
                # Allow this imported identity itself, then prove that its forbidden transitive edge is inspected.
                fixture_rules = json.loads((policy / "rules.json").read_text())
                fixture_rules[target]["direct"].append(intermediate)
                fixture_rules[target]["closure"][intermediate] = {"imported": True}
                (policy / "rules.json").write_text(json.dumps(fixture_rules))
            targets = list(dict.fromkeys(x for x in [target, intermediate, forbidden] if x))
            top = "cmake_minimum_required(VERSION 3.22)\nproject(execution_boundary LANGUAGES CXX)\nset(LUX_EDITOR_MIGRATION_STAGE " + args.stage + ")\n"
            directories = {}
            for value in targets:
                folder = root / locations[value]
                folder.mkdir(parents=True, exist_ok=True)
                (folder / "dummy.cpp").write_text("int " + value + "_fixture;\n")
                directories.setdefault(locations[value], []).append(value)
            for location, members in directories.items():
                (root / location / "CMakeLists.txt").write_text("".join(
                    (f"add_library({value} INTERFACE IMPORTED GLOBAL)\n" if value == "p10_imported_bridge" else
                     f"add_library({value} STATIC dummy.cpp)\n") + f"add_library(fixture::{value} ALIAS {value})\n"
                    for value in members))
                top += f"add_subdirectory({location})\n"
            edges = ""
            if intermediate:
                edges += f"target_link_libraries({target} PRIVATE fixture::{intermediate})\n"
            if forbidden:
                scope = "INTERFACE" if intermediate == "p10_imported_bridge" else "PRIVATE"
                edges += f'target_link_libraries({intermediate or target} {scope} "$<LINK_ONLY:fixture::{forbidden}>")\n'
            tail = f'include("{repo.as_posix()}/cmake/EditorArchitectureChecks.cmake")\nlux_editor_check_architecture()\n'
            (root / "CMakeLists.txt").write_text(top + edges + tail)
            probe = root / locations[target] / "probe.hpp"
            probe.write_text('auto build(ui::Root& root) { return std::make_unique<ui::Pane>(root, ui::PaneId{"bad"}, ui::PaneTypeId{"test"}, "bad"); }\n' if header == "ROOTED_FACTORY" else f"#include <{header}>\n" if header else "")
            build = root / "build"
            result = run([args.cmake, "-S", root, "-B", build, "-G", "Ninja"])
            output = result.stdout + result.stderr
            rule = target.upper() + ("_FORBIDDEN_INCLUDE" if header else "_FORBIDDEN_DEPENDENCY")
            if target == "ui_fixture": rule = "ENGINE_DEPENDS_ON_EDITOR"
            if header == "ROOTED_FACTORY": rule = "NEW_ROOTED_FACTORY"
            if name == "unused-old-editor": rule = "NEW_DEPENDS_ON_OLD"
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
