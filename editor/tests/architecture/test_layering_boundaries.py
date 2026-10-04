"""Real configure/build/reject/repair fixtures for the five-layer graph and header rules."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


def run(command, cwd=None):
    return subprocess.run(list(map(str, command)), cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--p11", action="store_true")
    parser.add_argument("--p12", action="store_true")
    parser.add_argument("--ec2", action="store_true")
    parser.add_argument("--ec3", action="store_true")
    parser.add_argument("--ec4", action="store_true")
    args = parser.parse_args()
    repo = args.source.resolve()
    base_rules = json.loads((repo / "editor/tests/architecture/rules.json").read_text())
    classification = base_rules["editor_layering"]["targets"]
    cases = [
        ("N01", "material_model", "material_ui", "authoring_outer_dependency", "include"),
        ("N02", "edit_sessions", "material_model", "editing_domain_dependency", "link"),
        ("N03", "editor_persistence", "scene_persistence", "persistence_policy_concrete_source", "link"),
        ("N04", "workspace_store", "view_api", "activity_workbench_dependency", "include"),
        ("N05", "editor_widgets", "edit_sessions", "widget_authoring_dependency", "include"),
        ("N06", "editor_tasks", "tasks_ui", "task_monitor_ui_dependency", "link"),
        ("N07", "material_ui", "scene_ui", "cross_tool_ui_dependency", "link"),
        ("N08", "material_interaction", "ui", "interaction_capability_leak", "transitive"),
        ("N09", "world_storage", "editor_contracts", "product_reverse_dependency", "link"),
        ("N10", "material_model", "material_ui", "authoring_outer_dependency", "transitive"),
        ("N11", "editor_persistence", "scene_persistence", "policy_instantiation_leak", "template"),
        ("N12", "scene_model", "editor_context", "new_legacy_dependency", "include"),
        ("N13", "scene_ui", "editor_scene_meta", "generated_provider_mismatch", "generated"),
        ("N14", "editor_contracts", "lux::cxx::core", "unclassified_dependency", "unknown"),
    ]
    if args.p11:
        cases = [
            ("N11-01-context", "editor_commands", "editor_context", "new_legacy_dependency", "include"),
            ("N11-01-ui", "editor_commands", "ui", "activity_ui_dependency", "include"),
            ("N11-02", "session_factories", "view_host", "activity_workbench_dependency", "include"),
            ("N11-03", "material_persistence", "editor_extensions", "activity_workbench_dependency", "transitive"),
            ("N11-04", "editor_extensions", "session_factories", "private_support_dependency", "private"),
            ("N11-05", "material_model", "process_execution", "authoring_outer_dependency", "link"),
            ("EC1-extension-gpu", "editor_extensions", "scene_ui", "extension_optional_capability_leak", "link"),
            ("EC1-extension-toolchain", "editor_extensions", "flowforge_compilation", "extension_optional_capability_leak", "transitive"),
        ]
    if args.p12:
        assert not args.p11
        # The retired target has no production source. A classified fixture permits the positive
        # build while the real checker must still reject linking it, even through LINK_ONLY.
        classification["editor_context"]["path"] = "fixture/retired_context"
        classification["editor_ui"]["path"] = "fixture/retired_ui"
        cases = [
            ("N12-01-context", "editor_bootstrap", "editor_context", "new_legacy_dependency", "transitive"),
            ("N12-01-ui", "editor_bootstrap", "editor_ui", "new_legacy_dependency", "link"),
            ("N12-02", "material_model", "material_ui", "authoring_outer_dependency", "transitive"),
            ("N12-03", "material_ui", "scene_ui", "cross_tool_ui_dependency", "link"),
            ("N12-04", "editor_tasks", "tasks_ui", "task_monitor_ui_dependency", "link"),
            ("N12-05", "world_storage", "editor_contracts", "product_reverse_dependency", "link"),
        ]
    if args.ec2:
        assert not args.p11 and not args.p12
        cases = [
            ("EC2-01-direct", "scene_script_assets", "editor_storage", "product_reverse_dependency", "link"),
            ("EC2-01-indirect", "scene_script_assets", "editor_persistence", "product_reverse_dependency", "transitive"),
            ("EC2-02", "scene_script_runtime", "edit_sessions", "product_reverse_dependency", "include"),
            ("EC2-03", "scene_execution", "scene_ui", "activity_workbench_dependency", "include"),
            ("EC2-04", "scene_script_assets", "edit_sessions", "product_reverse_dependency", "include"),
            ("EC2-04-lua", "scene_script_assets", "script_lua", "native_script_language_dependency", "lua-header"),
            ("EC2-05", "script_lua", "scene_script_skeleton", "generic_script_domain_dependency", "link"),
            ("EC2-05-core", "script_core", "toolchain_material_compiler", "generic_script_domain_dependency", "link"),
            ("EC2-06", "scene_script_assets", "script_lua", "native_script_language_dependency", "link"),
            ("EC2-07", "scene_script_assets_lua", "scene_script_assets", "script_projection_execution_dependency", "source"),
            ("EC2-08", "project_tools_ui", "editor_bootstrap", "workbench_application_dependency", "include"),
            ("EC2-09", "scene_script_assets", "scene_script_runtime", "data_behavior_dependency", "data"),
        ]
    if args.ec3:
        cases = [
            ("EC3-save-policy", "editor_storage", "editor_bootstrap", "activity_workbench_dependency", "include"),
            ("EC3-workspace", "workspace_store", "desktop_shell", "activity_workbench_dependency", "transitive"),
            ("EC3-settings-data", "layout_model", "project_tools_ui", "authoring_outer_dependency", "link"),
            ("EC3-viewport", "editor_viewport", "editor_bootstrap", "workbench_application_dependency", "include"),
        ]
    if args.ec4:
        cases = [
            ("EC4-object-editor", "object", "editor_contracts", "product_reverse_dependency", "include"),
            ("EC4-object-ui", "object", "ui", "framework_business_dependency", "link"),
            ("EC4-object-process", "object", "process_execution", "framework_business_dependency", "include"),
        ]
    folder_name = "ec4-boundaries" if args.ec4 else ("ec3-boundaries" if args.ec3 else ("ec2-boundaries" if args.ec2 else (
        "p12-boundaries" if args.p12 else ("p11-boundaries" if args.p11 else "layering-boundaries")))
    )
    folder = args.build / folder_name
    suffix = 1
    while folder.exists():
        folder = args.build / f"{folder_name}-{suffix}"
        suffix += 1
    folder.mkdir()
    results = []
    for case, owner, dependency, rule, kind in cases:
        root = folder / case
        root.mkdir(exist_ok=True)
        assert run(["git", "init", root]).returncode == 0
        tools = root / "editor/tests/architecture"
        tools.mkdir(parents=True, exist_ok=True)
        for file in ("check_editor_boundaries.py", "editor_layering.py"):
            shutil.copyfile(repo / "editor/tests/architecture" / file, tools / file)
        names = {owner, dependency}
        if kind == "transitive":
            names.add("editor_contracts")
        if kind == "generated":
            names.add("scene_fields_transform_ir_generate")
        declarations, rules = {}, json.loads(json.dumps(base_rules))
        # Preserve all real classifications. Only this named imported fixture leaf is added for
        # the positive control; removing its classification must fail without relying on linking.
        if kind == "unknown":
            rules["editor_layering"]["targets"][dependency] = {
                "layer": "EXTERNAL", "role": "VALUE", "capabilities": ["CPU"], "imported": True}
        for name in sorted(names):
            entry = rules["editor_layering"]["targets"][name]
            location = entry.get("path", "fixture/imported")
            directory = root / location
            directory.mkdir(parents=True, exist_ok=True)
            if kind == "unknown" and name == dependency:
                text = f"add_library({name} INTERFACE IMPORTED GLOBAL)\n"
            elif entry["role"] == "GENERATOR":
                output = "${CMAKE_CURRENT_BINARY_DIR}/generated/Generated.hpp"
                (directory / (name + ".in")).write_text("#pragma once\nstruct GeneratedValue {};\n")
                text = (f'add_custom_command(OUTPUT "{output}"\n'
                        f' COMMAND ${{CMAKE_COMMAND}} -E make_directory "${{CMAKE_CURRENT_BINARY_DIR}}/generated"\n'
                        f' COMMAND ${{CMAKE_COMMAND}} -E copy_if_different "${{CMAKE_CURRENT_SOURCE_DIR}}/{name}.in" "{output}"\n'
                        f' DEPENDS "${{CMAKE_CURRENT_SOURCE_DIR}}/{name}.in")\n'
                        f'add_custom_target({name} DEPENDS "{output}")\n')
            else:
                unit = directory / (name + ".cpp")
                unit.write_text(f"int {name}_value = 0;\n")
                text = f"add_library({name} STATIC {name}.cpp)\n"
                rules["editor_layering"]["files"][unit.relative_to(root).as_posix()] = [name]
            declarations.setdefault(location, []).append(text)
        for location, text in declarations.items():
            (root / location / "CMakeLists.txt").write_text("".join(text))
        include = root / classification[dependency]["path"] / "include/layering/Foreign.hpp" if kind in ("include", "template") else None
        if kind == "private":
            include = root / "editor/activities/sessions/sinclude/lux/engine/editor/detail/PrepareSession.hpp"
        if include:
            include.parent.mkdir(parents=True, exist_ok=True)
            include.write_text("#pragma once\nstruct ForeignValue {};\n")
            rules["editor_layering"]["files"][include.relative_to(root).as_posix()] = [dependency]
        unit = root / classification[owner]["path"] / (owner + ".cpp")
        original = unit.read_text()
        top = ('cmake_minimum_required(VERSION 3.22)\nproject(layering_fixture LANGUAGES CXX)\n'
               'set(CMAKE_CXX_STANDARD 20)\nset(CMAKE_CXX_EXTENSIONS OFF)\n'
               'set(LUX_EDITOR_MIGRATION_STAGE P10Q CACHE STRING "")\n'
               'set(LUX_EDITOR_LAYERING_MODE STRICT CACHE STRING "")\n')
        if args.p11:
            top = top.replace("STAGE P10Q", "STAGE P11")
        elif args.p12:
            top = top.replace("STAGE P10Q", "STAGE P12")
        elif args.ec2:
            top = top.replace("STAGE P10Q", "STAGE EC2")
        elif args.ec3:
            top = top.replace("STAGE P10Q", "STAGE EC3")
        elif args.ec4:
            top = top.replace("STAGE P10Q", "STAGE EC4")
        top += "".join(f"add_subdirectory({p})\n" for p in declarations)
        legal_edges = f"target_link_libraries({owner} PRIVATE {dependency})\n" if kind == "unknown" else ""
        if kind == "generated":
            legal_edges = f"add_dependencies({owner} scene_fields_transform_ir_generate)\n"
        tail = f'include("{repo.as_posix()}/cmake/EditorArchitectureChecks.cmake")\nlux_editor_check_architecture()\n'
        rules_file = tools / "rules.json"
        rules_file.write_text(json.dumps(rules, indent=2))
        (root / "CMakeLists.txt").write_text(top + legal_edges + tail)
        build = root / "build"

        def configure():
            return run([args.cmake, "-S", root, "-B", build, "-G", "Ninja"])

        positive = configure()
        positive_build = run([args.cmake, "--build", build, "--target", "all", "-j", "4", "--", "-k", "0"]) if positive.returncode == 0 else positive
        changed = ""
        if kind == "source":
            unit.write_text(original + "\n#include <fstream>\nvoid bad() { std::ifstream file(\"asset\"); }\n")
        elif kind == "lua-header":
            unit.write_text(original + "\n#include <lua.h>\n")
        elif kind == "data":
            data = rules["editor_layering"]["script_boundaries"]["data_headers"][0]
            header = root / data
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#pragma once\n#include <BehaviorSystem.hpp>\n')
            rules["editor_layering"]["files"][data] = [owner]
            rules_file.write_text(json.dumps(rules, indent=2))
        elif include:
            include_name = "lux/engine/editor/detail/PrepareSession.hpp" if kind == "private" else "layering/Foreign.hpp"
            unit.write_text(f"#include <{include_name}>\n" +
                           ("template<class T> int size() { return sizeof(T); }\nint instantiation = size<ForeignValue>();\n" if kind == "template" else original))
            changed = f'target_include_directories({owner} PRIVATE "{include.parent.parent.as_posix()}")\n'
        elif kind == "transitive":
            changed = (f"target_link_libraries({owner} PRIVATE editor_contracts)\n"
                       f'target_link_libraries(editor_contracts PUBLIC "$<LINK_ONLY:{dependency}>")\n')
        elif kind == "generated":
            changed = f"add_dependencies({owner} {dependency})\n"
        elif kind == "unknown":
            del rules["editor_layering"]["targets"][dependency]
            rules_file.write_text(json.dumps(rules, indent=2))
        else:
            changed = f'target_link_libraries({owner} PUBLIC "$<LINK_ONLY:{dependency}>")\n'
        (root / "CMakeLists.txt").write_text(top + legal_edges + changed + tail)
        negative = configure()
        extra_negatives = []
        if kind == "unknown":
            rules["editor_layering"]["targets"][dependency] = {
                "layer": "EXTERNAL", "role": "VALUE", "capabilities": ["CPU"], "imported": True}
            rules_file.write_text(json.dumps(rules, indent=2))
            for label, injection in [
                ("unclassified-target", 'add_library(layering_new_target INTERFACE)\n'),
                ("unresolved-import", f'target_link_libraries({owner} PUBLIC "missing::imported")\n'),
            ]:
                (root / "CMakeLists.txt").write_text(top + legal_edges + injection + tail)
                result = configure()
                extra_negatives.append((label, result))
        unit.write_text(original)
        if kind == "data":
            header.write_text("#pragma once\n")
        if kind == "unknown":
            rules["editor_layering"]["targets"][dependency] = {
                "layer": "EXTERNAL", "role": "VALUE", "capabilities": ["CPU"], "imported": True}
            rules_file.write_text(json.dumps(rules, indent=2))
        (root / "CMakeLists.txt").write_text(top + legal_edges + tail)
        repaired = configure()
        repaired_build = run([args.cmake, "--build", build, "--target", "all", "-j", "4", "--", "-k", "0"]) if repaired.returncode == 0 else repaired
        passed = (positive.returncode == 0 and positive_build.returncode == 0 and
                  negative.returncode != 0 and rule in negative.stdout + negative.stderr and
                  repaired.returncode == 0 and repaired_build.returncode == 0)
        passed = passed and all(r.returncode != 0 and rule in r.stdout + r.stderr for _, r in extra_negatives)
        logs = {}
        for phase, result in [("positive", positive), ("positive-build", positive_build),
                              ("negative", negative), ("repaired", repaired), ("repaired-build", repaired_build)] + extra_negatives:
            name = f"{case}/{phase}.log"
            (folder / name).write_text(result.stdout + result.stderr)
            logs[phase] = {"exit_code": result.returncode, "log": name}
        results.append({"id": case, "passed": passed, "expected_rule": rule, "phases": logs})
        print("PASS" if passed else "FAIL", case, flush=True)
    (folder / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    return 0 if all(x["passed"] for x in results) else 1


if __name__ == "__main__":
    sys.exit(main())
