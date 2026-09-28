"""Narrow V4 source/build checks. Not an AST, ownership or ABI verifier.

0 = no listed violations, 1 = violations, 2 = invalid input/tool failure.
Conditional target edges are checked conservatively across all configurations.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


def check_foundations(repo, targets, rules, sources, report):
    foundations = rules["foundation_targets"]
    definitions = {x["name"]: x for x in rules["targets"]}
    external = rules["foundation_external_targets"]

    def allowed_internal(name):
        result = set()
        pending = list(definitions[name]["dependencies"])
        while pending:
            dependency = pending.pop()
            if dependency not in result:
                result.add(dependency)
                pending.extend(definitions[dependency]["dependencies"])
        return result

    for name, policy in foundations.items():
        internal = allowed_internal(name)
        direct = set(definitions[name]["dependencies"]) | set(policy["external_dependencies"])
        allowed = internal | set(policy["external_closure"])
        pending = [(name, [name])]
        visited = set()
        while pending:
            current, chain = pending.pop()
            if current in visited:
                continue
            visited.add(current)
            node = targets.get(current)
            if not node:
                continue  # Absent foundation target is not implemented in this fixture/profile.
            for edge in filter(None, node.get("edges", "").split(";")):
                kind, dependency = edge.split(":", 1)
                detail = " -> ".join(chain + [dependency]) + " (" + kind + ")"
                dest = targets.get(dependency)
                if not dest:
                    report("FOUNDATION_UNRESOLVED_DEPENDENCY", name, detail)
                    continue
                if dependency not in allowed:
                    report("FOUNDATION_FORBIDDEN_DEPENDENCY", name, detail)
                elif dependency in external:
                    requirement = external[dependency]
                    if requirement.get("imported") and dest.get("IMPORTED") not in ["TRUE", "1"]:
                        report("FOUNDATION_DEPENDENCY_IDENTITY", name, detail + " must be imported")
                    if "path" in requirement:
                        expected = (repo / requirement["path"]).resolve()
                        if Path(dest["SOURCE_DIR"]).resolve() != expected:
                            report("FOUNDATION_DEPENDENCY_IDENTITY", name, detail + " has wrong source directory")
                if current == name and dependency not in direct:
                    report("FOUNDATION_DIRECT_DEPENDENCY", name, detail)
                pending.append((dependency, chain + [dependency]))

        prefixes = []
        headers = set(rules["foundation_standard_headers"])
        for dependency in internal | {name}:
            prefixes.extend(foundations[dependency]["public_prefixes"])
            headers.update(foundations[dependency]["public_headers"])
        for dependency in policy["external_closure"]:
            prefixes.extend(external[dependency]["include_prefixes"])
            headers.update(external[dependency]["include_headers"])
        scope = definitions[name]["path"] + "/"
        for path, source in sources.items():
            if not path.startswith(scope):
                continue
            for match in re.finditer(r'^\s*#\s*include\s*([<"])([^>"\n]+)', source, re.M):
                delimiter, header = match.groups()
                is_local = delimiter == '"' and (repo / path).parent.joinpath(header).resolve().is_relative_to(
                    (repo / scope).resolve())
                if header not in headers and not header.startswith(tuple(prefixes)) and not is_local:
                    report("FOUNDATION_FORBIDDEN_INCLUDE", path, header)


def inspect(repo, records, rules, stage, compile_db=None):
    findings = []

    def report(rule, path, detail):
        findings.append({"rule": rule, "path": str(path), "detail": detail})

    def relative(path):
        return Path(path).resolve().relative_to(repo).as_posix()

    targets = {r["name"]: r for r in records}
    new_targets = {t["name"] for t in rules["targets"]}
    # The executable name is reused at P12; its current app target is not a new module.
    for name, debt in rules.get("reused_targets", {}).items():
        current = targets.get(name)
        if current and stage < debt["deadline"] and relative(current["SOURCE_DIR"]) == debt["source"]:
            new_targets.discard(name)
    paths = subprocess.check_output(
        ["git", "-C", str(repo), "ls-files", "-c", "-o", "--exclude-standard", "-z"]
    ).decode("utf-8").split("\0")
    sources = {}
    for name in sorted(set(filter(None, paths))):
        path = repo / name
        if path.is_symlink():
            report("SYMLINK_REQUIRES_REVIEW", name, "Do not follow source links outside the inventory")
            continue
        if path.is_file() and (path.suffix in rules["source_extensions"] or path.name == "CMakeLists.txt"):
            # Evidence is not production code (and may quote retired identifiers).
            if not name.startswith("dev_log/"):
                sources[name] = path.read_text(encoding="utf-8-sig")
    for rule in rules["expired_paths"]:
        if stage >= rule["deadline"] and (repo / rule["path"]).exists():
            report("EXPIRED_PATH", rule["path"], rule["deadline"])
    for rule in rules["forbidden_definitions"]:
        if stage >= rule["deadline"]:
            for path, source in sources.items():
                if re.search(rule["pattern"], source):
                    report(rule["id"], path, rule["description"])

    if stage >= "P01":
        retired = re.compile(r'\b(beginSave|finishSave|ESaveOutcome|initially_saved|SAVE_STARTED|'
                             r'SAVE_IN_PROGRESS|STALE_SAVE|SaveTicket)\b')
        for path, source in sources.items():
            if not path.endswith((".hpp", ".cpp", ".h", ".cc")):
                continue
            # The sole intentional occurrence checks absence through a requires expression.
            if path == "editor/sessions/test/sessions.cpp":
                source = source.replace("value.beginSave();", "")
            if retired.search(source):
                report("HISTORY_PERSISTENCE_API", path, "P01 retired persistence declaration or call")
            if path.startswith("editor/history/") and re.search(r'\b(saved|save_pending|clean|pending|request)\b', source):
                report("HISTORY_PERSISTENCE_STATE", path, "Persistence state must not live in history")

    if stage >= "P01":
        check_foundations(repo, targets, rules, sources, report)

    scopes = tuple(rules["new_scopes"])
    forbidden_headers = set(rules["new_scope_forbidden_include"])
    for path, source in sources.items():
        if path.startswith(rules["transition_root"]):
            if stage >= rules["transition_delete_by"] or path not in rules["transition_allowlist"]:
                report("TRANSITION_NOT_ALLOWED", path, stage)
        if not path.startswith(scopes):
            continue
        for header in re.findall(r'^\s*#\s*include\s*[<"]([^>"\n]+)', source, re.M):
            if "LegacyPersistenceState" in header or "/transition/" in header:
                report("NEW_DEPENDS_ON_TRANSITION", path, header)
            if header in forbidden_headers:
                report("NEW_DEPENDS_ON_OLD", path, header)
            if "/pinclude/" in header or "/sinclude/" in header:
                report("PRIVATE_INCLUDE", path, header)
            if "/model/" in path and header.startswith("lux/engine/ui/"):
                report("MODEL_DEPENDS_ON_UI", path, header)

    for name, record in targets.items():
        try:
            source_dir = relative(record["SOURCE_DIR"])
        except ValueError:
            continue  # External dependencies are not assigned invented Lux layers.
        if name in new_targets:
            for key in ("INCLUDE_DIRECTORIES", "INTERFACE_INCLUDE_DIRECTORIES"):
                for include in record.get(key, "").split(";"):
                    if not include or "$<" in include:
                        continue  # Actual resolved flags are checked from the compile database too.
                    private = "/sinclude" in include or "/pinclude" in include
                    own = include.startswith(record["SOURCE_DIR"].rstrip("/") + "/")
                    if private and (not own or key.startswith("INTERFACE")):
                        report("PRIVATE_INCLUDE", name, include)
        if not source_dir.startswith("engine/") and name not in new_targets:
            continue
        queue = [(name, [name])]
        seen = set()
        while queue:
            current, chain = queue.pop()
            if current in seen:
                continue
            seen.add(current)
            node = targets.get(current, {})
            for edge in filter(None, node.get("edges", "").split(";")):
                kind, dependency = edge.split(":", 1)
                dest = targets.get(dependency)
                if not dest:
                    continue
                try:
                    location = relative(dest["SOURCE_DIR"])
                except ValueError:
                    continue
                chain_next = chain + [dependency]
                detail = " -> ".join(chain_next) + " (" + kind + ")"
                if source_dir.startswith("engine/") and location.startswith("editor/"):
                    report("ENGINE_DEPENDS_ON_EDITOR", name, detail)
                if name == "editor_workflows" and dependency == "editor_bootstrap":
                    report("WORKFLOWS_DEPEND_ON_BOOTSTRAP", name, detail)
                if name.endswith("_model") and (location.startswith("modules/function/ui") or
                                               location.startswith("editor/ui")):
                    report("MODEL_DEPENDS_ON_UI", name, detail)
                if name in new_targets and dependency in rules["legacy_targets"]:
                    report("NEW_DEPENDS_ON_OLD_TARGET", name, detail)
                queue.append((dependency, chain_next))

    if compile_db:
        for unit in json.loads(compile_db.read_text(encoding="utf-8")):
            try:
                path = relative(unit["file"])
            except ValueError:
                continue
            if not path.startswith(scopes):
                continue
            command = unit.get("command", " ".join(unit.get("arguments", []))).replace("\\", "/")
            own = next(t["path"] for t in rules["targets"] if path.startswith(t["path"] + "/"))
            for private in re.findall(r'[^\s";]*(?:/pinclude|/sinclude)[^\s";]*', command):
                if "/" + own + "/" not in private:
                    report("PRIVATE_COMPILE_INCLUDE", path, private)
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--stage", default="P00", choices=[f"P{i:02}" for i in range(14)])
    parser.add_argument("--rules", type=Path, default=Path(__file__).with_name("rules.json"))
    parser.add_argument("--compile-db", type=Path)
    args = parser.parse_args()
    try:
        findings = inspect(args.repo.resolve(), json.loads(args.graph.read_text(encoding="utf-8")),
                           json.loads(args.rules.read_text(encoding="utf-8")), args.stage, args.compile_db)
        print(json.dumps({"status": "FAIL" if findings else "PASS", "findings": findings}, ensure_ascii=False))
        return int(bool(findings))
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"AUDIT_ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
