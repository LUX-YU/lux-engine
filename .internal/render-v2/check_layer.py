"""Layering gate protection checker; does not modify historical qualifications."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--snapshot", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = args.source.resolve()
snapshot = json.loads(args.snapshot.read_text(encoding="utf-8-sig"))
base = snapshot["baseline"]

def git(repo, *arguments):
    return subprocess.check_output(["git", "-C", str(repo), *arguments], text=True).strip()

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

allowed = ("modules/function/render/", "cmake/render-v2-bootstrap/", ".internal/render-v2/")
changed = set(git(source, "diff", "--name-only", base).splitlines())
changed.update(git(source, "ls-files", "--others", "--exclude-standard").splitlines())
assert all(name.startswith(allowed) for name in changed), sorted(name for name in changed if not name.startswith(allowed))
manifest_path = "docs/render-v2/LEGACY_SOURCE_MANIFEST.json"
assert not git(source, "diff", base, "--", manifest_path)
manifest = json.loads((source / manifest_path).read_text())
assert len(manifest["files"]) == 719
for record in manifest["files"]:
    assert git(source, "hash-object", record["destination"]) == record["blob"], record["destination"]
    assert git(source, "rev-parse", base + ":" + record["destination"]) == record["blob"]
user = Path("E:/SyncForder/CodeRepos/lux-engine")
assert git(user, "rev-parse", "HEAD") == snapshot["user_head"]
assert git(user, "status", "--porcelain") == snapshot["user_status"]
for name, expected in snapshot["user_sha256"].items():
    assert digest(user / name) == expected, name
for name, expected in snapshot["dependencies"].items():
    repo = user.parent / name
    assert git(repo, "rev-parse", "HEAD") == expected["sha"], name
    assert git(repo, "status", "--porcelain") == expected["status"], name
assert not git(source, "diff", base, "--", ".internal/editor-redesign", "docs/render-v2", "modules/platform/ipc")
result = dict(baseline=base, head=git(source, "rev-parse", "HEAD"), legacy_blobs=719,
              protected_user_files=snapshot["user_sha256"], dependencies=snapshot["dependencies"],
              changed_files=sorted(changed), status="PASS")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(dict(status="PASS", legacy=719, changed=len(changed))))
