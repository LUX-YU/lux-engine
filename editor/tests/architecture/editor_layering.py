"""Five-layer checks over the existing CMake graph and compiler dependency evidence.

The source inventory and target classifications live in rules.json, not in this checker.
No unknown target, generated provider or construction exception is an implicit leaf.
"""
from pathlib import Path
import json
import re


def check(repo, records, sources, rules, mode, report, compiler_dependencies=None):
    if mode not in ("CONSTRUCTION", "STRICT"):
        raise ValueError("unknown editor layering mode: " + str(mode))
    policy = rules["editor_layering"]
    declared = policy["targets"]
    actual = {r["name"]: r for r in records}
    files = policy["files"]
    native = set(policy["native_libraries"])
    formal = {name for name, value in declared.items() if value["layer"] in ("E0", "E1", "E2", "E3", "E4")}
    exceptions = policy["construction_exceptions"] if mode == "CONSTRUCTION" else []

    def issue(rule, owner, detail):
        if not any(x == {"rule": rule, "owner": owner, "detail": detail} for x in exceptions):
            report(rule, owner, detail)

    def relative(value):
        p = Path(value).resolve()
        return p.relative_to(repo.resolve()).as_posix() if p.is_relative_to(repo.resolve()) else None

    for name, node in actual.items():
        entry = declared.get(name)
        if not entry:
            issue("unclassified_dependency", name, "target has no reviewed layer/role/capability")
            continue
        source = relative(node["SOURCE_DIR"])
        if "path" in entry and source != entry["path"]:
            issue("provider_identity_mismatch", name, str(source) + " != " + entry["path"])
        if entry.get("imported") and node.get("IMPORTED") not in ("1", "TRUE"):
            issue("provider_identity_mismatch", name, "expected the selected imported target")
        for edge in filter(None, node.get("edges", "").split(";")):
            dependency = declared.get(edge.split(":", 1)[1], {})
            if dependency.get("layer") == "RETAINED" and name not in dependency["consumers"]:
                issue("retained_consumer_mismatch", name, edge)

    for path in sources:
        formal_path = path.startswith(("editor/authoring/", "editor/activities/", "editor/workbench/", "editor/application/"))
        if formal_path and Path(path).suffix in (".cpp", ".hpp", ".h", ".cc"):
            if path not in files and path not in policy["shared_headers"]:
                issue("unclassified_dependency", path, "source/header has no explicit provider")

    def violation(owner, dependency, generated=False):
        origin = declared[owner]
        target = declared[dependency]
        layer, dest = origin["layer"], target["layer"]
        caps = set(target["capabilities"])
        if layer == "ENGINE" and dest in ("E0", "E1", "E2", "E3", "E4"):
            return "product_reverse_dependency"
        if layer == "ENGINE" and dest == "RETAINED":
            return "product_reverse_dependency"
        if owner in formal and dest == "RETAINED":
            return "generated_provider_mismatch" if generated else "new_legacy_dependency"
        if generated:
            return None  # Build tools are not runtime capabilities; their identity still matters.
        if layer == "E0" and dest in ("E1", "E2", "E3", "E4"):
            return "editing_domain_dependency"
        if layer in ("E0", "E1") and (dest in ("E2", "E3", "E4") or
                caps.intersection(("PROCESS", "TOOLCHAIN", "GPU", "GUI"))):
            return "authoring_outer_dependency" if layer == "E1" else "editing_capability_leak"
        if owner == "editor_tasks" and (dest in ("E3", "E4") or "GUI" in caps or "GPU" in caps):
            return "task_monitor_ui_dependency"
        if layer == "E2" and dest in ("E3", "E4"):
            return "activity_workbench_dependency"
        if owner in ("editor_commands", "session_factories") and caps.intersection(("GUI", "GPU")):
            return "activity_ui_dependency"
        if owner == "editor_persistence" and (dependency in policy["concrete_save_providers"] or
                caps.intersection(("PROCESS", "TOOLCHAIN", "GPU", "GUI", "PLATFORM"))):
            return "persistence_policy_concrete_source"
        if origin["role"] == "INTERACTION" and caps.intersection(("GUI", "GPU")):
            return "interaction_capability_leak"
        if owner == "editor_widgets" and dependency in policy["author_state_providers"]:
            return "widget_authoring_dependency"
        if owner in ("material_ui", "flow_ui") and dependency == "scene_ui":
            return "cross_tool_ui_dependency"
        if owner in formal and dest == "TEST":
            return "production_test_dependency"
        return None

    def edges(name):
        for edge in filter(None, actual.get(name, {}).get("edges", "").split(";")):
            yield edge.split(":", 1)

    closures = {}
    for owner in actual:
        if owner not in declared or declared[owner]["layer"] in ("TEST", "RETAINED", "EXTERNAL"):
            continue
        pending, visited = [(owner, [owner], False)], set()
        while pending:
            name, chain, generated = pending.pop()
            if (name, generated) in visited:
                continue
            visited.add((name, generated))
            for kind, dependency in edges(name):
                via_generator = generated or kind == "MANUALLY_ADDED_DEPENDENCIES"
                detail = " -> ".join(chain + [dependency]) + " (" + kind + ")"
                if dependency not in actual:
                    if dependency not in native:
                        issue("unclassified_dependency", owner, detail)
                    continue
                if dependency not in declared:
                    issue("unclassified_dependency", owner, detail)
                    continue
                broken = violation(owner, dependency, via_generator)
                if broken:
                    issue(broken, owner, detail)
                pending.append((dependency, chain + [dependency], via_generator))
        closures[owner] = {name for name, _ in visited}

    # A source file may be compiled into a test as well as its provider, but one definition
    # cannot silently become a second production implementation after relocation.
    owned = {name: set() for name in actual}
    for name, node in actual.items():
        for key in ("SOURCES", "INTERFACE_SOURCES"):
            for source in filter(None, node.get(key, "").split(";")):
                if "$<" in source:
                    continue  # Resolved generated paths are checked in compiler dependency evidence.
                p = Path(source)
                if not p.is_absolute():
                    p = Path(node["SOURCE_DIR"]) / p
                path = relative(p)
                if path:
                    owned[name].add(path)
                    if name in formal and path.startswith("editor/") and path in files and name not in files[path]:
                        issue("source_provider_mismatch", name, path + ": " + str(files[path]))
    logical = {}
    for path in set(files) | set(policy["shared_headers"]):
        if "/include/" in path or "/sinclude/" in path or "/pinclude/" in path:
            header = re.split(r"/(?:sinclude|pinclude|include)/", path, maxsplit=1)[1]
            logical.setdefault(header, []).append(path)
    for path in sources:
        if path.startswith(("engine/", "modules/")) and "/include/" in path:
            logical.setdefault(path.split("/include/", 1)[1], []).append(path)
    for name, paths in owned.items():
        if name in formal:
            paths.update(path for path, providers in files.items() if name in providers)

    def inspect_header(owner, path, header_path, generated=False, template=False):
        shared = policy["shared_headers"].get(header_path)
        if shared is not None:
            if owner not in shared["consumers"]:
                issue("private_support_dependency", owner, path + " -> " + header_path)
            return
        providers = files.get(header_path)
        if not providers and header_path.startswith(("engine/", "modules/")):
            matches = [prefix for prefix in policy["header_roots"] if header_path.startswith(prefix)]
            if matches:
                providers = policy["header_roots"][max(matches, key=len)]
        if not providers:
            if header_path.startswith("editor/"):
                issue("unclassified_dependency", owner, "unowned Editor header: " + header_path)
            return
        # Shared test instantiation does not grant a production header another provider.
        providers = [p for p in providers if declared.get(p, {}).get("layer") != "TEST"]
        for dependency in providers:
            if dependency not in declared:
                issue("unclassified_dependency", owner, header_path + ": " + dependency)
                continue
            broken = violation(owner, dependency, generated)
            if broken:
                if template and owner == "editor_persistence":
                    broken = "policy_instantiation_leak"
                issue(broken, owner, path + " -> " + header_path + " (" + dependency + ")")

    for owner in formal.intersection(actual):
        for path in sorted(owned[owner]):
            source = sources.get(path)
            if source is None:
                continue
            for delimiter, header in re.findall(r'^\s*#\s*include\s*([<"])([^>"\n]+)', source, re.M):
                candidates = list(logical.get(header, []))
                if delimiter == '"':
                    local = relative((repo / path).parent / header)
                    if local and (repo / local).is_file():
                        candidates = [local]
                for included in candidates:
                    inspect_header(owner, path, included, template="template" in source)

    if compiler_dependencies:
        for unit in json.loads(Path(compiler_dependencies).read_text(encoding="utf-8-sig")):
            owner = unit["target"]
            if owner not in declared:
                issue("unclassified_dependency", owner, "compiler dependency owner is unknown")
                continue
            if owner not in formal:
                continue
            for include in unit["includes"]:
                path = relative(include)
                if path:
                    inspect_header(owner, unit["source"], path, template=True)
                else:
                    normalized = str(include).replace("\\", "/")
                    if "/editor/" in normalized and any(part in normalized for part in ("/gen/", "_gen/")):
                        matches = [p for p in policy["generated_roots"] if (p["suffix"] and normalized.endswith(p["suffix"])) or
                                   p["segment"] in normalized]
                        if len(matches) != 1:
                            issue("generated_provider_mismatch", owner, normalized)
                        elif matches[0].get("public"):
                            provider = matches[0]["owner"]
                            if provider not in closures.get(owner, set()) or violation(owner, provider):
                                issue("generated_provider_mismatch", owner, normalized)
                        elif matches[0]["owner"] != owner:
                            issue("generated_provider_mismatch", owner, normalized)
