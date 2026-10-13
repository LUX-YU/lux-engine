"""Read-only G2 snapshot, Legacy, dependency and installed-header audit."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


BASE = "9526b5efd61891b96cd8594122243db994f69729"


def git(root, *args, data=None):
    return subprocess.check_output(["git", "-C", str(root), *args], input=data)


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def tree(root, revision):
    result = {}
    for line in git(root, "ls-tree", "-r", revision).decode().splitlines():
        metadata, path = line.split("\t", 1)
        mode, _, blob = metadata.split()
        result[path] = (mode, blob)
    return result


def allowed(path):
    return (path.startswith("modules/function/render/data/")
            or path == "modules/function/render/CMakeLists.txt"
            or path in {"docs/render-v2/rebuild/G2_IMPLEMENTATION.md",
                        "docs/render-v2/rebuild/G2_VERIFICATION.md",
                        "docs/render-v2/rebuild/check_g2.py"})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--installed", type=Path)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--user-root", type=Path)
    parser.add_argument("--snapshot", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    manifest = json.loads((root / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text(encoding="utf-8"))
    baseline, current, frozen = tree(root, BASE), tree(root, "HEAD"), tree(root, manifest["source_sha"])
    files = manifest["files"]
    require(len(files) == 719, "Legacy manifest count changed")
    paths = "\n".join(entry["destination"] for entry in files) + "\n"
    hashes = git(root, "hash-object", "--stdin-paths", data=paths.encode()).decode().splitlines()
    require(len(hashes) == 719, "Incomplete Legacy worktree check")
    for entry, blob in zip(files, hashes):
        expected = (entry["mode"], entry["blob"])
        require(frozen.get(entry["source"]) == expected, "Frozen Legacy mismatch")
        require(current.get(entry["destination"]) == expected, "Committed Legacy mismatch")
        require(blob == entry["blob"], "Worktree Legacy mismatch: " + entry["destination"])
    guard = "render_legacy/CMakeLists.txt"
    require(current[guard] == baseline[guard], "Legacy guard changed")
    require(git(root, "hash-object", guard).decode().strip() == current[guard][1], "Legacy guard dirty")
    changed = git(root, "diff", "--name-only", BASE, "HEAD").decode().splitlines()
    require(all(allowed(path) for path in changed), "Committed change exceeds G2 boundary")
    status = git(root, "status", "--porcelain=v1", "--untracked-files=all").decode().splitlines()
    if args.clean:
        require(not status, "Verification source is not clean")
    for line in status:
        path = line[3:]
        require(allowed(path) or path == ".vscode/settings.json", "Unexpected worktree change: " + path)
    render = root / "modules/function/render"
    require({p.name for p in render.iterdir()} == {"core", "data", "CMakeLists.txt"}, "Later stage exists")
    for p in (render / "data").rglob("*"):
        if not p.is_file() or p.suffix not in {".hpp", ".cpp"} or "test" in p.parts:
            continue
        text = p.read_text(encoding="utf-8")
        for forbidden in ("unordered_map", "unordered_set", "dynamic_cast", "typeid(", "throw ", "catch (",
                          "vulkan/", "lux/engine/log", "std::cout", "std::cerr", "printf("):
            require(forbidden not in text, f"Forbidden production construct {forbidden}: {p}")
    report = {"head": git(root, "rev-parse", "HEAD").decode().strip(), "base": BASE,
              "legacy_blobs": 719, "guard": "unchanged", "changed_files": changed, "status": status}
    if args.installed:
        canonical = {
            "lux/engine/error/Error.hpp": root / "modules/core/error/include/lux/engine/error/Error.hpp",
            "lux/engine/error/ErrorDescriptor.hpp": root / "modules/core/error/include/lux/engine/error/ErrorDescriptor.hpp",
            "lux/engine/description/Image.hpp": root / "modules/resource/description/include/lux/engine/description/Image.hpp"
        }
        for module in ("core", "data"):
            directory = render / module / "include"
            for p in directory.rglob("*.hpp"):
                canonical[p.relative_to(directory).as_posix()] = p
        installed = args.installed / "include"
        actual = {p.relative_to(installed).as_posix() for p in installed.rglob("*") if p.is_file()}
        require(actual == set(canonical), "Installed header set incomplete or polluted")
        for name, source in canonical.items():
            require((installed / name).read_bytes() == source.read_bytes(), "Installed header differs: " + name)
        report["installed_headers"] = sorted(actual)
    if args.build:
        commands = json.loads((args.build / "compile_commands.json").read_text())
        require(commands, "No compile commands")
        for entry in commands:
            command = entry["command"].lower().replace("\\", "/")
            require("std:c++20" in command and "/gr-" in command, "C++20/no-RTTI missing")
            for forbidden in ("render_legacy", "vulkansdk", "/install/relwithdebinfo", "/render/transport/",
                              "/render/runtime/", "/render/native/", "/editor/", "/ecs/"):
                require(forbidden not in command, "Build dependency leak: " + forbidden)
        report["translation_units"] = len(commands)
    if args.user_root or args.snapshot:
        require(args.user_root and args.snapshot, "Both user-root and snapshot are needed")
        settings = (args.user_root / ".vscode/settings.json").read_bytes()
        require(settings == (args.snapshot / "settings.json").read_bytes(), "User settings changed")
        patch = git(args.user_root, "diff", "--binary", "HEAD", "--", ".vscode/settings.json")
        require(patch == (args.snapshot / "user-settings.patch").read_bytes(), "User patch changed")
        report["user_settings_sha256"] = hashlib.sha256(settings).hexdigest()
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
