"""S01 exact move, rule and real target/compile graph comparison."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--before-graph", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
a = parser.parse_args()
source = a.source.resolve()
before = "8ef17a1014ca02265c535d7855cf6848b8f9b936"
head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
def blob(sha, path):
    return subprocess.check_output(["git", "show", sha + ":" + path], cwd=source)
moves = [("editor/history", "editor/editing/history"), ("editor/sessions", "editor/editing/sessions")]
verified = []
for old, new in moves:
    assert not (source / old).exists()
    files = subprocess.check_output(["git", "ls-tree", "-r", "--name-only", before, old], cwd=source, text=True).splitlines()
    for path in files:
        destination = new + path[len(old):]
        content = blob(head, destination)
        if not path.endswith(("README.md", "test/scope_compile.py")):
            assert content == blob(before, path), path
        verified.append(destination)
    present = subprocess.check_output(["git", "ls-tree", "-r", "--name-only", head, new], cwd=source, text=True).splitlines()
    assert set(present) == {new + path[len(old):] for path in files}

rules_before = json.loads(blob(before, "editor/tests/architecture/rules.json"))
rules_after = json.loads(blob(head, "editor/tests/architecture/rules.json"))
def moved(value):
    if isinstance(value, dict): return {k: moved(v) for k,v in value.items()}
    if isinstance(value, list): return [moved(v) for v in value]
    if isinstance(value, str):
        for old,new in moves:
            if value == old or value.startswith(old + "/"): return new + value[len(old):]
    return value
expected = moved(rules_before)
expected["expired_paths"] += [dict(path=old, deadline="P02", id=key) for (old,new),key in
                             zip(moves, ["S01_HISTORY_MOVE", "S01_SESSIONS_MOVE"])]
assert rules_after == expected, "Rules changed beyond the exact move and two old-path expiry checks"

old_graph = {x["name"]:x for x in json.loads(a.before_graph.read_text())}
new_graph = {x["name"]:x for x in json.loads((a.build / "editor-architecture/targets.json").read_text())}
def closure(graph, root):
    result = {}; pending = [root]
    while pending:
        name = pending.pop()
        if name in result: continue
        row = graph[name]
        edges = sorted(filter(None, row["edges"].split(";")))
        result[name] = dict(type=row["TYPE"], edges=edges)
        pending.extend(edge.split(":",1)[1] for edge in edges)
    return result
for target in ["editor_contracts", "edit_history", "edit_sessions", "scene_model", "editor_editing"]:
    assert closure(old_graph, target) == closure(new_graph, target), target
commands = json.loads((a.build / "compile_commands.json").read_text())
compiled = [Path(row["file"]).resolve().as_posix() for row in commands]
for path in verified:
    if "/src/" in path and path.endswith(".cpp"):
        assert compiled.count((source / path).as_posix()) == 1, path
assert not any("/editor/history/" in path or "/editor/sessions/" in path for path in compiled)
assert not subprocess.check_output(["git", "diff", "3a4d6ce367cfe3b9551ccde01a0de6d7088fa256", head,
                                  "--", "dev_log/P00", "dev_log/P01", "dev_log/P01-R1", "dev_log/P02"], cwd=source)
a.output.write_text(json.dumps(dict(status="PASS", implementation_sha=head, moves=moves, files=verified,
    actual_closures_unchanged=True, unique_compile=True, immutable_receipts_unchanged=True), indent=2) + "\n")
print("PASS S01 exact move, unchanged real closures and SDK target definitions, unique source compilation, frozen receipts")
