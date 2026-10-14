"""Read-only IPC boundaries, protection, dependencies and installed header audit."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

BASE = "05e4d385d59b05a02baa017bc05feb1e3543508b"


def git(root, *args, data=None):
    return subprocess.check_output(["git", "-C", str(root), *args], input=data)


def require(value, message):
    if not value:
        raise SystemExit(message)


def tree(root, revision):
    result = {}
    for line in git(root, "ls-tree", "-r", revision).decode().splitlines():
        metadata, path = line.split("\t", 1)
        mode, _, blob = metadata.split()
        result[path] = (mode, blob)
    return result


def allowed(path):
    return (path.startswith("modules/platform/ipc/")
            or path.startswith("modules/function/render/runtime/connection/")
            or path in {"modules/platform/CMakeLists.txt", "modules/function/render/CMakeLists.txt",
                        "cmake/render-v2-bootstrap/CMakeLists.txt",
                        "docs/render-v2/rebuild/IPC_LIFECYCLE.md",
                        "docs/render-v2/rebuild/IPC_IMPLEMENTATION.md",
                        "docs/render-v2/rebuild/IPC_VERIFICATION.md",
                        "docs/render-v2/rebuild/check_ipc.py"})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--final", action="store_true")
    parser.add_argument("--build", type=Path)
    parser.add_argument("--installed", type=Path)
    parser.add_argument("--user-root", type=Path)
    parser.add_argument("--snapshot", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    base, current = tree(root, BASE), tree(root, "HEAD")
    for name, identity in base.items():
        if not allowed(name):
            require(current.get(name) == identity, "Protected commit changed: " + name)
    manifest = json.loads((root / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text())
    frozen = tree(root, manifest["source_sha"])
    require(len(manifest["files"]) == 719, "Legacy count")
    paths = "\n".join(item["destination"] for item in manifest["files"]) + "\n"
    hashes = git(root, "hash-object", "--stdin-paths", data=paths.encode()).decode().splitlines()
    for item, blob in zip(manifest["files"], hashes):
        identity = (item["mode"], item["blob"])
        require(frozen[item["source"]] == current[item["destination"]] == identity, "Legacy tree mismatch")
        require(blob == item["blob"], "Legacy worktree mismatch")
    changes = git(root, "diff", "--name-only", BASE, "HEAD").decode().splitlines()
    require(all(allowed(path) for path in changes), "Commit boundary violation")
    status = git(root, "status", "--porcelain=v1", "--untracked-files=all").decode().splitlines()
    if args.clean:
        require(not status, "Not a clean snapshot")
    for line in status:
        require(allowed(line[3:]) or line[3:] == ".vscode/settings.json", "Worktree boundary: " + line)
    report = {"head": git(root, "rev-parse", "HEAD").decode().strip(), "baseline": BASE,
              "legacy_blobs": 719, "historical_files": "unchanged", "changes": changes, "status": status}
    if args.final:
        require(not (root / "modules/function/render/runtime/connection").exists(), "Old connection remains")
    production = root / "modules/platform/ipc"
    for path in [*production.joinpath("src").rglob("*.cpp"), *production.joinpath("include").rglob("*.hpp")]:
        text = path.read_text(encoding="utf-8")
        for forbidden in ("std::mutex", "condition_variable", "shared_mutex", "atomic_flag", "std::thread",
                          "std::jthread", "std::shared_ptr", "unordered_map", "RequestCell", "owner_thread_id",
                          "lux/engine/render", "vulkan/", "stdexec", "dynamic_cast", "typeid("):
            require(forbidden not in text, f"Forbidden {forbidden}: {path}")
    if args.user_root:
        require(args.snapshot, "Missing user snapshot")
        actual = (args.user_root / ".vscode/settings.json").read_bytes()
        require(actual == (args.snapshot / "settings.json").read_bytes(), "User settings changed")
        report["settings_sha256"] = hashlib.sha256(actual).hexdigest()
    if args.build:
        commands = json.loads((args.build / "compile_commands.json").read_text())
        for item in commands:
            command = item["command"].replace("\\", "/").lower()
            require("std:c++20" in command and "/gr-" in command, "C++20/no-RTTI missing")
            for forbidden in ("render_legacy", "vulkansdk", "/install/relwithdebinfo", "/ecs/", "/editor/"):
                require(forbidden not in command, "Dependency leak: " + forbidden)
            if "/ipc/src/" in item["file"].replace("\\", "/"):
                require("/function/render/" not in command, "IPC production depends on Render")
        report["translation_units"] = len(commands)
    if args.installed:
        canonical = {}
        for module in ("core", "data", "protocol"):
            headers = root / "modules/function/render" / module / "include"
            canonical.update({p.relative_to(headers).as_posix(): p for p in headers.rglob("*.hpp")})
        headers = production / "include"
        canonical.update({p.relative_to(headers).as_posix(): p for p in headers.rglob("*.hpp")})
        for name in ("Error", "ErrorDescriptor"):
            canonical[f"lux/engine/error/{name}.hpp"] = root / f"modules/core/error/include/lux/engine/error/{name}.hpp"
        canonical["lux/engine/description/Image.hpp"] = root / "modules/resource/description/include/lux/engine/description/Image.hpp"
        generated = "lux/engine/platform/ipc/visibility.h"
        require(args.build, "Installed audit needs generated visibility source")
        canonical[generated] = args.build / "gen/include" / generated
        actual = {p.relative_to(args.installed / "include").as_posix()
                  for p in (args.installed / "include").rglob("*") if p.is_file()}
        require(actual == set(canonical), "Installed header set differs")
        for name, path in canonical.items():
            require((args.installed / "include" / name).read_bytes() == path.read_bytes(), "Installed bytes differ: " + name)
        report["installed_headers"] = {name: hashlib.sha256(path.read_bytes()).hexdigest()
                                       for name, path in canonical.items()}
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
