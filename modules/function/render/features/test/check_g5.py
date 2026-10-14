"""Read-only G5 protection and dependency audit. Store stdout outside the source tree."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--repo", type=Path, default=Path.cwd())
p.add_argument("--build", type=Path)
p.add_argument("--clean", action="store_true")
p.add_argument("--user-worktree", type=Path)
args = p.parse_args()
repo = args.repo.resolve()
baseline = "f80902e100879b114e848cabe1469540aba4d25d"


def git(*values, data=None, cwd=repo):
    return subprocess.check_output(["git", "-C", str(cwd), *values], input=data).decode("utf-8").strip()


def tree(revision):
    entries = git("ls-tree", "-rz", revision).split("\0")
    return {e.split("\t", 1)[1]: e.split("\t", 1)[0].split()[2] for e in entries if e}


def allowed(path):
    return path.startswith("modules/function/render/features/") or path in {
        "modules/function/render/CMakeLists.txt",
        "modules/function/render/graph/include/lux/engine/render/graph/Builder.hpp",
    }


status = git("status", "--porcelain")
if args.clean:
    assert not status, status
head = git("rev-parse", "HEAD")
old, current = tree(baseline), tree("HEAD")
changed = git("diff", "--name-only", baseline, "--").splitlines()
untracked = git("ls-files", "--others", "--exclude-standard").splitlines()
assert all(allowed(f) for f in changed + untracked), (changed, untracked)
protected = [path for path in old if not allowed(path)]
blobs = git("hash-object", "--stdin-paths", data=("\n".join(protected) + "\n").encode()).splitlines()
literal_blob_matches = []
for path, blob in zip(protected, blobs, strict=True):
    assert old[path] == current[path], path
    if blob != old[path]:
        # Some historical reports contain committed CRLF. Applying today's
        # autocrlf filter would change that blob even when the bytes are exact.
        raw = (repo / path).read_bytes()
        literal = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
        assert literal == old[path], path
        literal_blob_matches.append(path)
manifest = json.loads((repo / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text(encoding="utf-8"))
assert manifest["file_count"] == len(manifest["files"]) == 719
frozen = tree(manifest["source_sha"])
for entry in manifest["files"]:
    assert frozen[entry["source"]] == entry["blob"] == current[entry["destination"]], entry

user = {}
if args.user_worktree:
    for path, expected in {
        ".vscode/settings.json": "f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c",
        "modules/function/render/graph/include/lux/engine/render/graph/Graph.hpp":
            "00c28abc7bd0a786666f101a5212400a5b467af6bdbbc5049c69d59d548c726a",
    }.items():
        user[path] = hashlib.sha256((args.user_worktree / path).read_bytes()).hexdigest()
        assert user[path] == expected, path

module = repo / "modules/function/render/features"
for source in [*module.glob("src/*.cpp"), *module.glob("include/**/*.hpp")]:
    text = source.read_text(encoding="utf-8")
    for forbidden in ["unordered_map", "unordered_set", "dynamic_cast", "typeid(", "std::function",
                      "#include <vulkan", "#include <lux/engine/platform/ipc", "render_legacy",
                      "printf(", "lux::log", "FeatureManager", "StorageRegistry"]:
        assert forbidden not in text, (source, forbidden)
    assert "throw " not in text and "catch (" not in text, source

compiled = []
if args.build:
    database = json.loads((args.build / "compile_commands.json").read_text(encoding="utf-8"))
    for entry in database:
        if "/features/" not in entry["file"].replace("\\", "/"):
            continue
        command = entry["command"].replace("\\", "/").lower()
        for forbidden in ["render_legacy", "vulkansdk", "platform/ipc", "render/runtime", "engine/ecs", "stdexec"]:
            assert forbidden not in command, (entry["file"], forbidden)
        assert "c++20" in command and ("/gr-" in command or "-fno-rtti" in command), entry
        compiled.append(entry["file"])
    assert compiled
    expected = ("public_links=lux::engine::render::render_graph;lux::engine::render::render_data;"
                "lux::engine::render::render_protocol")
    assert (args.build / "render/features/test/feature-dependencies.txt").read_text().strip() == expected
    graph = (args.build / "render/graph/test/graph-dependencies.txt").read_text().strip()
    assert graph == "public_links=lux::engine::render::render_core"

print(json.dumps({"head": head, "baseline": baseline, "legacy_blobs": 719,
                  "protected_files": len(protected), "working_tree": status,
                  "literal_blob_matches": literal_blob_matches,
                  "user_sha256": user, "changes": changed + untracked,
                  "feature_compile_units": compiled, "protection": "PASS",
                  "dependency_audit": "PASS" if args.build else "SOURCE_ONLY"},
                 indent=2, ensure_ascii=False))
