"""v11 protection audit; does not alter any historical checker or qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

BASELINE = "98798da75a201933e6950ef5631444fc199e783a"


def git(root, *args, data=None):
    return subprocess.check_output(["git", "-C", str(root), *args], input=data).decode().strip()


def tree(root, revision):
    return {line.split("\t")[1]: line.split("\t")[0].split()[2]
            for line in git(root, "ls-tree", "-rz", revision).split("\0") if line}


def allowed(path):
    if path.startswith(".internal/editor-redesign/"):
        return Path(path).name.startswith("RENDER_V11_") or Path(path).name == "check_render_v11.py"
    if path.startswith(("modules/platform/ipc/", "cmake/render-v2-bootstrap/")):
        return True
    if path in ("modules/function/render/CMakeLists.txt", "cmake/InstallComponents.cmake"):
        return True
    if path.startswith("modules/function/render/"):
        return path.split("/")[3] in {"core", "data", "protocol", "features", "graph", "resource", "native", "runtime"}
    # Only the evidenced synchronous-completion repair and its direct tests may touch Engine execution.
    return path in {
        "engine/process/execution/include/lux/engine/process/PortSender.hpp",
        "engine/process/execution/test/PortSenderTest.cpp",
        "engine/process/execution/CMakeLists.txt",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--user-source", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.source.resolve()
    snapshot = json.loads(args.snapshot.read_text(encoding="utf-8-sig"))
    assert snapshot["baseline"] == BASELINE
    baseline, current = tree(root, BASELINE), tree(root, "HEAD")
    changed = set(git(root, "diff", "--name-only", BASELINE).splitlines())
    changed.update(git(root, "ls-files", "--others", "--exclude-standard").splitlines())
    for path in changed:
        assert allowed(path), f"Outside v11 whitelist: {path}"
        if path in baseline and path.endswith((".md", ".py")):
            assert "/test/" in path or path.startswith("cmake/"), f"Historical evidence modified: {path}"
    manifest = json.loads((root / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text())
    frozen = tree(root, manifest["source_sha"])
    assert len(manifest["files"]) == 719
    names = [item["destination"] for item in manifest["files"]]
    hashes = git(root, "hash-object", "--stdin-paths", data=("\n".join(names) + "\n").encode()).splitlines()
    for item, filtered in zip(manifest["files"], hashes, strict=True):
        name = item["destination"]
        raw = (root / name).read_bytes()
        literal = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
        assert item["blob"] == baseline[name] == current[name] == frozen[item["source"]], name
        assert item["blob"] in (filtered, literal), name
    for name, digest in snapshot["user_sha256"].items():
        assert hashlib.sha256((args.user_source / name).read_bytes()).hexdigest() == digest, name
    assert git(args.user_source, "status", "--porcelain") == snapshot["user_status"]
    assert git(args.user_source, "rev-parse", "HEAD") == snapshot["user_head"]
    for name, state in snapshot["dependencies"].items():
        dependency = args.user_source.parent / name
        assert git(dependency, "rev-parse", "HEAD") == state["sha"], name
        assert git(dependency, "status", "--porcelain") == state["status"], name
    subprocess.run(["git", "-C", str(root), "diff", "--check"], check=True)
    result = {"baseline": BASELINE, "head": git(root, "rev-parse", "HEAD"), "legacy": 719,
              "changed": sorted(changed), "status": "PASS (protection only)"}
    if args.output:
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
