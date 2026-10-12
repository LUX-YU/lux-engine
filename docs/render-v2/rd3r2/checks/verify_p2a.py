"""Read-only P2-A boundary/provenance checks; GPU behavior is tested separately."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

BASE = "98002bb03bb8ae0c6ed156fcd7e847082b6a7148"
FROZEN = "a669409a289a6fa4092f21176397795b1cdb7f3e"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--implementation", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.source.resolve()
    output = args.output.resolve()
    if output.is_relative_to(root):
        raise SystemExit("Evidence must be outside source")
    output.mkdir(parents=True, exist_ok=True)
    checks = []

    def check(name, value):
        checks.append({"name": name, "passed": bool(value)})

    def git(*values):
        return subprocess.check_output(["git", "-C", str(root), *values])

    def tree(ref):
        result = {}
        for record in git("ls-tree", "-rlz", ref).split(b"\0"):
            if record:
                meta, name = record.split(b"\t")
                mode, _, blob, size = meta.split()
                result[name.decode()] = (mode.decode(), blob.decode(), int(size))
        return result

    check("exact I", git("rev-parse", "HEAD").decode().strip() == args.implementation)
    check("clean", not git("status", "--porcelain", "--untracked-files=all"))
    before, current, frozen = tree(BASE), tree("HEAD"), tree(FROZEN)
    changes = sorted(p for p in before.keys() | current.keys() if before.get(p) != current.get(p))
    native = "modules/function/render/vulkan/"
    allowed_headers = {
        native + "include/lux/engine/render/vulkan/memory/Memory.hpp",
        native + "include/lux/engine/render/vulkan/transfer/Submission.hpp",
    }
    for name in changes:
        allowed = (name.startswith("modules/function/render/runtime/")
                   or name.startswith("docs/render-v2/rd3r2/")
                   or name == "cmake/render-v2-bootstrap/RD3Runtime.cmake"
                   or name in allowed_headers)
        check("allowed " + name, allowed)
    check("no deleted files", not (before.keys() - current.keys()))
    for name, value in before.items():
        protected = (name.startswith("docs/") or name.startswith("render_legacy/")
                     or name.startswith(native + "src/")
                     or name.startswith("modules/function/render/graph/src/"))
        if protected:
            check("protected " + name, current.get(name) == value)
    manifest = json.loads((root / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text())
    check("719 frozen sources", len(manifest["files"]) == manifest["file_count"] == 719)
    for entry in manifest["files"]:
        value = (entry["mode"], entry["blob"], entry["bytes"])
        check("frozen " + entry["source"], frozen.get(entry["source"]) == value)
        check("legacy " + entry["destination"], current.get(entry["destination"]) == value)
    for name, (_, blob, _) in current.items():
        data = (root / name).read_bytes()
        digest = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        check("exact checkout " + name, digest == blob)
    git_dir = Path(git("rev-parse", "--absolute-git-dir").decode().strip()).resolve()
    common = Path(git("rev-parse", "--git-common-dir").decode().strip())
    common = (common if common.is_absolute() else root / common).resolve()
    check("independent git dir", git_dir == common and common.is_relative_to(root))
    check("no alternates", not (common / "objects/info/alternates").exists())
    objects = [p for p in (common / "objects").rglob("*") if p.is_file()]
    check("no hardlinked objects", bool(objects) and max(p.stat().st_nlink for p in objects) == 1)
    failures = [item["name"] for item in checks if not item["passed"]]
    result = {"implementation": args.implementation, "base": BASE, "status": "FAIL" if failures else "PASS",
              "checks": len(checks), "failures": failures, "changes": changes, "legacy_count": 719}
    (output / "checks.json").write_text(json.dumps(checks, indent=2) + "\n")
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
