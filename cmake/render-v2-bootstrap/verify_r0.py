"""Verify R0 against immutable Git trees; write evidence outside the checkout.

Python 3.10+. This checker does not build V1, install an SDK, or qualify rendering.
Run only from the clean implementation commit, before the verification commit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


BASE = "a669409a289a6fa4092f21176397795b1cdb7f3e"
CLOSURES = (
    "modules/function/render",
    "engine/context/rendering",
    "engine/scene/builtin_systems/render",
    "modules/function/ui/rendering",
    "engine/project/plugins/rendering",
    "examples/render-plugin",
)
GUARD = 'message(FATAL_ERROR "Frozen Render V1 reference; build its pinned SHA in a separate worktree")\n'


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def tree(root, revision):
    result = {}
    for record in git(root, "ls-tree", "-rlz", revision).split(b"\0"):
        if not record:
            continue
        info, path = record.split(b"\t", 1)
        mode, kind, oid, size = info.decode().split()
        result[path.decode()] = {"mode": mode, "kind": kind, "blob": oid, "bytes": int(size) if size != "-" else None}
    return result


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bootstrap-build", type=Path)
    args = parser.parse_args()
    root = args.source.resolve()
    output = args.output.resolve()
    check(not output.is_relative_to(root), "Evidence must be outside the source checkout")
    check(not git(root, "status", "--porcelain", "--untracked-files=all"), "Source is not clean")
    head = git(root, "rev-parse", "HEAD").decode().strip()
    ancestry = subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", BASE, head], check=False)
    check(ancestry.returncode == 0, "R0 implementation does not descend from the frozen base")
    before, after = tree(root, BASE), tree(root, head)
    manifest_bytes = git(root, "show", head + ":docs/render-v2/LEGACY_SOURCE_MANIFEST.json")
    manifest = json.loads(manifest_bytes)
    expected = {p: v for p, v in before.items() if any(p.startswith(c + "/") for c in CLOSURES)}
    check(manifest["source_sha"] == BASE, "Wrong manifest base")
    check(manifest["closures"] == list(CLOSURES), "Wrong closure roots")
    check(len(expected) == 719 and sum(v["bytes"] for v in expected.values()) == 38439280, "Wrong frozen totals")
    check(manifest["file_count"] == 719 and manifest["total_blob_bytes"] == 38439280, "Wrong manifest totals")
    rows = manifest["files"]
    check([r["source"] for r in rows] == sorted(expected), "Missing, duplicate, or unordered manifest row")
    for row in rows:
        source = row["source"]
        destination = "render_legacy/" + source
        check(row["destination"] == destination, "Wrong relocation destination")
        check({k: row[k] for k in ("mode", "blob", "bytes")} ==
              {k: expected[source][k] for k in ("mode", "blob", "bytes")}, "Wrong manifest row: " + source)
        check(source not in after, "Old source retained: " + source)
        check(after.get(destination) == expected[source], "Frozen file differs: " + source)
    legacy = {p for p in after if p.startswith("render_legacy/")}
    check(legacy == {"render_legacy/" + p for p in expected} | {"render_legacy/CMakeLists.txt"}, "Extra/missing archive file")
    check(git(root, "show", head + ":render_legacy/CMakeLists.txt").decode() == GUARD, "Archive guard changed")

    changed = sorted(p for p in before.keys() | after.keys() if before.get(p) != after.get(p))
    for path in changed:
        allowed = path in expected or path.startswith(("render_legacy/", "docs/render-v2/", "cmake/render-v2-bootstrap/"))
        check(allowed, "Forbidden changed path: " + path)
    new_cpp = [p for p in after.keys() - before.keys()
               if not p.startswith("render_legacy/") and Path(p).suffix.lower() in (".cpp", ".cc", ".cxx", ".c", ".hpp", ".h")]
    check(not new_cpp, "New production C/C++ present")
    receipts = [p for p in before if p.startswith("docs/editor-mechanism-")]
    check(len(receipts) == 47 and all(after.get(p) == before[p] for p in receipts), "Historical documents changed")

    inputs = json.loads(git(root, "show", head + ":docs/render-v2/INPUT_MANIFEST.json"))
    check(len(inputs["documents"]) == 12, "Wrong document count")
    for item in inputs["documents"]:
        data = git(root, "show", head + ":docs/render-v2/" + item["repository_name"])
        check(hashlib.sha256(data).hexdigest() == item["sha256"], "Input document changed: " + item["repository_name"])

    # Inspect tracked active source/build inputs, excluding archives and evidence scripts.
    scanned = 0
    for path in after:
        if path.startswith(("render_legacy/", "editor_legacy/", ".internal/", "docs/")):
            continue
        if Path(path).suffix.lower() not in (".cmake", ".cpp", ".cc", ".cxx", ".c", ".hpp", ".h") and not path.endswith("CMakeLists.txt"):
            continue
        data = (root / path).read_text(encoding="utf-8-sig")
        check("render_legacy" not in data.replace("\\", "/"), "Active legacy reference: " + path)
        scanned += 1
    bootstrap = (root / "cmake/render-v2-bootstrap/CMakeLists.txt").read_text()
    check(not re.search(r"\b(add_(?:subdirectory|library|executable)|find_package|install|target_link_libraries)\s*\(", bootstrap),
          "R0 bootstrap contains a production/import/install operation")

    model_result = None
    if args.bootstrap_build:
        build = args.bootstrap_build.resolve()
        index_files = sorted((build / ".cmake/api/v1/reply").glob("index-*.json"))
        check(bool(index_files), "Missing CMake File API reply")
        index = json.loads(index_files[-1].read_bytes())
        reply = index_files[-1].parent
        models = {o["kind"]: json.loads((reply / o["jsonFile"]).read_bytes()) for o in index["objects"]}
        check({"codemodel", "cmakeFiles", "toolchains"} <= models.keys(), "Incomplete File API request")
        model = models["codemodel"]
        targets = [t for c in model["configurations"] for t in c.get("targets", [])]
        production = []
        for target in targets:
            detail = json.loads((reply / target["jsonFile"]).read_bytes())
            if detail["type"] != "UTILITY":
                production.append(target["name"])
            check("render_legacy" not in json.dumps(detail).replace("\\\\", "/"), "Legacy in File API target")
        check(not production, "Bootstrap has production targets")
        check(Path(models["cmakeFiles"]["paths"]["source"]).resolve() == root / "cmake/render-v2-bootstrap", "Wrong CMake source root")
        for item in models["cmakeFiles"]["inputs"]:
            check("render_legacy" not in item["path"].replace("\\", "/"), "Legacy CMake input")
        ninja = (build / "build.ninja").read_text()
        check("render_legacy" not in ninja and "build install:" not in ninja, "Unexpected legacy/install build edge")
        check(not (build / "install_manifest.txt").exists(), "Unexpected install manifest")
        model_result = {"production_targets": production, "utility_targets": len(targets),
                        "cmake_input_count": len(models["cmakeFiles"]["inputs"]), "toolchains": models["toolchains"]}
    result = {"status": "PASS", "base_sha": BASE, "implementation_sha": head, "frozen_files": len(expected),
              "frozen_blob_bytes": 38439280, "source_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
              "active_inputs_scanned": scanned, "historical_documents_preserved": len(receipts),
              "historical_documents_deleted": [], "new_production_cpp": new_cpp, "file_api": model_result,
              "v1_frozen_sha_matrix": "NOT_RUN", "v2_product": "EXPECTED_UNAVAILABLE",
              "render_behavior_tests": "NOT_RUN_NO_R0_PRODUCTION_TARGET"}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
