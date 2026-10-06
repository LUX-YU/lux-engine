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
    parser.add_argument("--stage", choices=["P08", "P10", "P10Q"], default="P08")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--evidence", required=True, type=Path)
    args = parser.parse_args()
    repo = args.source.resolve()
    rules = json.loads((repo / "editor_legacy/tests/architecture/rules.json").read_text())
    locations = {x["name"]: x["path"] for x in rules["targets"]}
    locations.update(editor_storage_legacy="editor_legacy/activities/project", editor_context="editor_legacy/context", ui_fixture="modules/function/ui", process_execution="engine/process/execution")
    locations.update(scene_composition="engine/scene/composition", render_runtime="modules/function/render/runtime")
    locations["view_api_legacy"] = "fixture/retired_view_api"
    locations["view_host_legacy"] = "fixture/retired_view_host"
    cases = [
        ("scene-author-interaction", "scene_model_legacy", None, "scene_interaction_legacy", ""),
        ("material-author-interaction", "material_model_legacy", None, "material_interaction_legacy", ""),
        ("flow-author-interaction", "flowforge_model_legacy", None, "flowforge_interaction_legacy", ""),
        ("scene-context", "scene_interaction_legacy", None, "editor_context", ""),
        ("material-transitive-context", "material_interaction_legacy", "material_model_legacy", "editor_context", ""),
        ("flow-transitive-ui", "flowforge_interaction_legacy", "flowforge_model_legacy", "ui_fixture", ""),
        ("scene-ui", "scene_interaction_legacy", None, "ui_fixture", ""),
        ("scene-run-transitive-render-runtime", "scene_interaction_legacy", "scene_execution_api_legacy", "render_runtime", ""),
        ("view-scene", "editor_composition_legacy", None, "scene_model_legacy", ""),
        ("view-storage", "editor_composition_legacy", None, "editor_storage_legacy", ""),
        ("view-render", "editor_composition_legacy", None, "render_runtime", ""),
        ("view-transitive-model", "editor_composition_legacy", "editor_contracts_legacy", "material_model_legacy", ""),
        ("engine-ui-editor", "ui_fixture", None, "editor_composition_legacy", ""),
        ("scene-old-bridge", "scene_interaction_legacy", None, None, "SceneRunCaptureAccess.hpp"),
        ("view-model-include", "editor_composition_legacy", None, None, "lux/engine/editor/scene/SceneSession.hpp"),
        ("view-rooted-factory", "editor_composition_legacy", None, None, "ROOTED_FACTORY"),
        ("view-rooted-base", "editor_composition_legacy", None, None, "ROOTED_BASE"),
        ("view-root-reader-legal", "editor_composition_legacy", None, None, "ROOT_READER"),
        ("scene-legal", "scene_interaction_legacy", "scene_model_legacy", None, ""),
        ("material-legal", "material_interaction_legacy", "material_model_legacy", None, ""),
        ("flow-legal", "flowforge_interaction_legacy", "flowforge_model_legacy", None, ""),
        ("view-legal", "editor_composition_legacy", "editor_contracts_legacy", None, ""),
    ]
    if args.stage in ("P10", "P10Q"):
        cases = [
            ("host-scene-owner", "editor_composition_legacy", None, "scene_model_legacy", ""),
            ("host-run-owner", "editor_composition_legacy", None, "scene_execution_legacy", ""),
            ("host-old-editor", "editor_composition_legacy", None, "editor_context", ""),
            ("widgets-runtime", "editor_widgets_legacy", None, "scene_composition", ""),
            ("widgets-model", "editor_widgets_legacy", None, "material_model_legacy", ""),
            ("scene-old-ui", "scene_ui_legacy", None, "editor_ui", ""),
            ("material-transitive-old", "material_ui_legacy", "scene_ui_legacy", "editor_context", ""),
            ("flow-old-editor", "flow_ui_legacy", None, "editor_flowforge", ""),
            ("harness-transitive-old", "editor_scene_views_test_legacy", "material_ui_legacy", "editor_context", ""),
            ("harness-imported-old", "editor_scene_views_test_legacy", "p10_imported_bridge", "editor_context", ""),
            ("engine-ui-new-editor", "ui_fixture", None, "scene_ui_legacy", ""),
            ("model-new-ui", "scene_model_legacy", None, "scene_ui_legacy", ""),
            ("view-api-desktop", "editor_composition_legacy", None, "desktop_shell_legacy", ""),
            ("project-storage", "project_ui_legacy", None, "editor_storage_legacy", ""),
            ("host-model-header", "editor_composition_legacy", None, None, "lux/engine/editor/scene/SceneSession.hpp"),
            ("scene-old-header", "scene_ui_legacy", None, None, "lux/engine/editor/scene/SceneEditor.hpp"),
            ("widgets-private-header", "editor_widgets_legacy", None, None, "../../tools/scene/pinclude/SceneEditorData.hpp"),
            ("scene-rooted-factory", "scene_ui_legacy", None, None, "ROOTED_FACTORY"),
            ("scene-rooted-base", "scene_ui_legacy", None, None, "ROOTED_BASE"),
            ("scene-root-reader-legal", "scene_ui_legacy", None, None, "ROOT_READER"),
            ("host-legal", "editor_composition_legacy", "editor_contracts_legacy", None, ""),
            ("desktop-legal", "desktop_shell_legacy", "editor_composition_legacy", None, ""),
            ("widgets-legal", "editor_widgets_legacy", None, None, ""),
            ("scene-legal", "scene_ui_legacy", "scene_interaction_legacy", None, ""),
            ("material-legal", "material_ui_legacy", "material_interaction_legacy", None, ""),
            ("flow-legal", "flow_ui_legacy", "flowforge_interaction_legacy", None, ""),
            ("harness-legal", "editor_scene_views_test_legacy", "scene_ui_legacy", None, ""),
        ]
        locations.update(editor_ui="editor_legacy/ui", editor_flowforge="editor_legacy/tools/flowforge",
                         scene_execution_legacy="editor_legacy/activities/scene", p10_imported_bridge="external/p10")
    if args.stage == "P10Q":
        cases.extend([
            ("scene-retired-host", "scene_ui_legacy", None, "view_host_legacy", ""),
            ("material-retired-host", "material_ui_legacy", None, "view_host_legacy", ""),
            ("flow-retired-api", "flow_ui_legacy", None, "view_api_legacy", ""),
            ("scene-retired-header", "scene_ui_legacy", None, None, "lux/engine/editor/views/IViewHost.hpp"),
            ("material-retired-header", "material_ui_legacy", None, None, "lux/engine/editor/desktop/ViewHost.hpp"),
            ("flow-retired-header", "flow_ui_legacy", None, None, "lux/engine/editor/views/ViewFactory.hpp"),
            ("scene-ui-error-legal", "scene_ui_legacy", None, None, "lux/engine/editor/desktop/UiError.hpp"),
            ("material-ui-error-legal", "material_ui_legacy", None, None, "lux/engine/editor/desktop/UiError.hpp"),
            ("flow-ui-error-legal", "flow_ui_legacy", None, None, "lux/engine/editor/desktop/UiError.hpp"),
            ("viewport-model", "editor_viewport_legacy", None, "scene_model_legacy", ""),
            ("viewport-inspector", "editor_viewport_legacy", None, "scene_ui_legacy", ""),
            ("viewport-context", "editor_viewport_legacy", None, "editor_context", ""),
            ("material-scene-ui", "material_ui_legacy", None, "scene_ui_legacy", ""),
            ("material-old-editing-header", "material_ui_legacy", None, None, "lux/engine/editor/EditorError.hpp"),
            ("tasks-desktop-production-header", "tasks_ui_legacy", None, None, "lux/engine/editor/desktop/ViewHost.hpp"),
            ("viewport-legal", "editor_viewport_legacy", None, None, ""),
        ])
    evidence = []
    for name, target, intermediate, forbidden, header in cases:
        with tempfile.TemporaryDirectory(prefix="lux-p08-boundary-") as directory:
            root = Path(directory)
            assert run(["git", "init", root]).returncode == 0
            policy = root / "editor_legacy/tests/architecture"
            policy.mkdir(parents=True)
            for file in ["rules.json", "check_editor_boundaries.py"]:
                shutil.copyfile(repo / "editor_legacy/tests/architecture" / file, policy / file)
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
            source_probes = {
                "ROOTED_FACTORY": 'auto build(ui::Root& root) { return std::make_unique<ui::Pane>(root, ui::PaneId{"bad"}, ui::PaneTypeId{"test"}, "bad"); }\n',
                "ROOTED_BASE": 'struct Derived : ui::Pane { Derived(ui::Root& parent) : Pane(parent, {}, {}, "bad") {} };\n',
                "ROOT_READER": 'struct Derived : ui::Pane { Derived(ObjectDispatcherRef dispatcher) : Pane(dispatcher, {}, {}, "detached") {} void inspect(ui::Root& root) { (void)root.findPane({}); } };\n',
            }
            probe.write_text(source_probes.get(header, f"#include <{header}>\n" if header else ""))
            provider = root / locations[target] / "CMakeLists.txt"
            provider.write_text(provider.read_text() + f'target_sources({target} PRIVATE "{probe.as_posix()}")\n')
            build = root / "build"
            result = run([args.cmake, "-S", root, "-B", build, "-G", "Ninja"])
            output = result.stdout + result.stderr
            rule = target.upper() + ("_FORBIDDEN_INCLUDE" if header else "_FORBIDDEN_DEPENDENCY")
            if target == "ui_fixture": rule = "ENGINE_DEPENDS_ON_EDITOR"
            if header in ("ROOTED_FACTORY", "ROOTED_BASE"): rule = "NEW_ROOTED_FACTORY"
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
