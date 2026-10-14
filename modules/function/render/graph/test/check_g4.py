"""G4 read-only protection/dependency audit; evidence belongs outside the source tree."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--repo", type=Path, default=Path.cwd())
parser.add_argument("--baseline", default="4d0d6378558174622d8a9a7d6aca63fb1d608284")
parser.add_argument("--build", type=Path)
parser.add_argument("--clean", action="store_true")
parser.add_argument("--user-settings", action="store_true")
args = parser.parse_args()
repo = args.repo.resolve()


def git(*values, data=None):
    return subprocess.check_output(["git", "-C", str(repo), *values], input=data).decode("utf-8").strip()


def tree(revision):
    entries = git("ls-tree", "-rz", revision).split("\0")
    return {entry.split("\t", 1)[1]: entry.split("\t", 1)[0].split()[2]
            for entry in entries if entry}


status = git("status", "--porcelain")
if args.clean:
    assert not status, status
head = git("rev-parse", "HEAD")
manifest = json.loads((repo / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text(encoding="utf-8"))
assert manifest["file_count"] == len(manifest["files"]) == 719
current = tree("HEAD")
frozen = tree(manifest["source_sha"])
paths = [entry["destination"] for entry in manifest["files"]]
working = git("hash-object", "--stdin-paths", data=("\n".join(paths) + "\n").encode()).splitlines()
for entry, blob in zip(manifest["files"], working, strict=True):
    assert frozen[entry["source"]] == entry["blob"], entry
    assert current[entry["destination"]] == blob == entry["blob"], entry

changed = git("diff", "--name-only", args.baseline, "--").splitlines()
for path in changed:
    allowed = path.startswith("modules/function/render/graph/") or path == "modules/function/render/CMakeLists.txt"
    if path == ".vscode/settings.json" and args.user_settings:
        allowed = True
    assert allowed, f"Out-of-stage change: {path}"
settings_hash = hashlib.sha256((repo / ".vscode/settings.json").read_bytes()).hexdigest()
if args.user_settings:
    assert settings_hash == "f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c"

graph = repo / "modules/function/render/graph"
for source in [*graph.glob("src/*"), *graph.glob("include/**/*.hpp")]:
    text = source.read_text(encoding="utf-8")
    for forbidden in ["unordered_map", "unordered_set", "dynamic_cast", "typeid(", "std::function",
                      "std::shared_ptr", "#include <vulkan", "#include <lux/engine/platform/ipc",
                      "#include <lux/engine/render/protocol", "#include <lux/engine/render/data",
                      "printf(", "lux::log", "render_legacy"]:
        assert forbidden not in text, (source, forbidden)

compiled = []
if args.build:
    database = json.loads((args.build / "compile_commands.json").read_text(encoding="utf-8"))
    for command in database:
        file = command["file"].replace("\\", "/")
        if "/graph/" not in file:
            continue
        text = command["command"].replace("\\", "/").lower()
        for forbidden in ["render_legacy", "vulkansdk", "platform/ipc", "render/protocol",
                          "render/data", "render/runtime", "engine/ecs", "stdexec"]:
            assert forbidden not in text, (file, forbidden)
        assert "c++20" in text and ("/gr-" in text or "-fno-rtti" in text), file
        compiled.append(file)
    assert compiled, "No Graph compile commands"
    assert (args.build / "render/graph/test/graph-dependencies.txt").read_text().strip() == (
        "public_links=lux::engine::render::render_core")

print(json.dumps({"head": head, "baseline": args.baseline, "legacy_blobs": 719,
                  "working_tree": status, "settings_sha256": settings_hash,
                  "changed": changed, "graph_compile_units": compiled,
                  "protection": "PASS", "dependency_audit": "PASS"}, indent=2, ensure_ascii=False))
