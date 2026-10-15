"""G11A-R source protection. Historical checkers remain bound to their original commits."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

BASELINE = "d5b4ff0ad792dd8e2d9ada7c26bda3c86fc33959"


def git(root, *args, data=None):
    return subprocess.check_output(["git", "-C", str(root), *args], input=data).decode().strip()


def tree(root, revision):
    return {item.split("\t")[1]: item.split("\t")[0].split()[2]
            for item in git(root, "ls-tree", "-rz", revision).split("\0") if item}


def allowed(path):
    parts = path.split("/")
    if path.startswith(".internal/editor-redesign/"):
        return parts[-1].startswith("G11A_R_") or parts[-1] == "check_render_g11ar.py"
    if path.startswith("modules/platform/gapi/") or path.startswith("cmake/render-v2-bootstrap/"):
        return True
    if path in ("modules/function/render/CMakeLists.txt", "cmake/InstallComponents.cmake"):
        return True
    return path.startswith("modules/function/render/") and parts[3] in {
        "core", "data", "protocol", "graph", "features", "resource", "native", "runtime"
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--user-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--extension", action="store_true")
    args = parser.parse_args()
    root = args.source.resolve()
    snapshot = json.loads(args.snapshot.read_text(encoding="utf-8-sig"))
    assert snapshot["baseline"] == BASELINE
    original = tree(root, BASELINE)
    changed = set(git(root, "diff", "--name-only", BASELINE).splitlines())
    changed.update(git(root, "ls-files", "--others", "--exclude-standard").splitlines())
    for path in changed:
        assert allowed(path), f"Outside G11A-R boundary: {path}"
        if path in original and path.endswith((".md", ".py")):
            assert not path.startswith((".internal/", "docs/")), f"Historical evidence modified: {path}"
            assert "/test/" in path or path.startswith("cmake/"), f"Historical report modified: {path}"
    manifest = json.loads((root / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text())
    frozen = tree(root, manifest["source_sha"])
    current = tree(root, "HEAD")
    assert len(manifest["files"]) == 719
    names = [item["destination"] for item in manifest["files"]]
    filtered = git(root, "hash-object", "--stdin-paths", data=("\n".join(names) + "\n").encode()).splitlines()
    for item, blob in zip(manifest["files"], filtered, strict=True):
        name = item["destination"]
        raw = (root / name).read_bytes()
        literal = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
        assert item["blob"] == original[name] == current[name] == frozen[item["source"]], name
        assert item["blob"] in (blob, literal), name
    for name, digest in snapshot["user_sha256"].items():
        assert hashlib.sha256((args.user_source / name).read_bytes()).hexdigest() == digest, name
    assert git(args.user_source, "status", "--porcelain") == snapshot["user_status"]
    assert git(args.user_source, "rev-parse", "HEAD") == snapshot["user_head"]
    for name, state in snapshot["dependencies"].items():
        dependency = args.user_source.parent / name
        assert git(dependency, "rev-parse", "HEAD") == state["sha"], name
        assert git(dependency, "status", "--porcelain") == state["status"], name
    subprocess.run(["git", "-C", str(root), "diff", "--check"], check=True)
    if args.extension:
        extension_base = "6ad87a920bc1194caca5d7626e6d59c3e5a36766"
        extension_changes = set(git(root, "diff", "--name-only", extension_base).splitlines())
        extension_changes.update(git(root, "ls-files", "--others", "--exclude-standard").splitlines())
        for path in extension_changes:
            assert "/test/" in path or path.startswith(".internal/editor-redesign/"), f"Extension changed core: {path}"
    result = {"baseline": BASELINE, "head": git(root, "rev-parse", "HEAD"), "legacy": 719,
              "user_sha256": snapshot["user_sha256"], "dependencies": snapshot["dependencies"],
              "changed": sorted(changed), "status": "PASS (protection only)"}
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"status": result["status"], "legacy": 719, "changed_files": len(changed)}))


if __name__ == "__main__":
    main()
