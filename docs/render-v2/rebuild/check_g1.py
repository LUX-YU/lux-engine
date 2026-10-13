"""Read-only G0/G1 boundary, Legacy blob and installed-header checks."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def git(root, *args, data=None):
    return subprocess.run(
        ["git", "-C", str(root), *args], input=data, check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    ).stdout


def tree(root, revision):
    result = {}
    for row in git(root, "ls-tree", "-r", "-z", revision).split(b"\0"):
        if row:
            metadata, name = row.split(b"\t", 1)
            mode, kind, blob = metadata.decode().split()
            result[name.decode()] = (mode, blob)
    return result


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--base", required=True)
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--installed", type=Path)
    parser.add_argument("--user-root", type=Path)
    parser.add_argument("--snapshot", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    manifest = json.loads((root / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text(encoding="utf-8"))
    frozen = tree(root, manifest["source_sha"])
    current = tree(root, "HEAD")
    files = manifest["files"]
    require(len(files) == 719, "Legacy manifest count changed")
    inputs = "\n".join(entry["destination"] for entry in files) + "\n"
    hashes = git(root, "hash-object", "--stdin-paths", data=inputs.encode()).decode().splitlines()
    require(len(hashes) == len(files), "Incomplete worktree blob scan")
    for entry, blob in zip(files, hashes):
        expected = (entry["mode"], entry["blob"])
        require(frozen.get(entry["source"]) == expected, "Frozen source mismatch: " + entry["source"])
        require(current.get(entry["destination"]) == expected, "HEAD mismatch: " + entry["destination"])
        require(blob == entry["blob"], "Worktree mismatch: " + entry["destination"])
    guard = "render_legacy/CMakeLists.txt"
    require(current[guard] == tree(root, args.base)[guard], "Legacy guard changed")
    require(git(root, "hash-object", guard).decode().strip() == current[guard][1], "Legacy guard is dirty")

    def allowed(path):
        return (path.startswith("modules/function/render/core/") or path.startswith("docs/render-v2/rebuild/")
                or path in {"modules/function/render/CMakeLists.txt", "cmake/render-v2-bootstrap/CMakeLists.txt"})

    changed = git(root, "diff", "--name-only", args.base, "HEAD").decode().splitlines()
    require(all(allowed(path) for path in changed), "Commit changes exceed G1 boundary")
    render = root / "modules/function/render"
    require({p.name for p in render.iterdir()} == {"core", "CMakeLists.txt"}, "Later stage exists")
    status = git(root, "status", "--porcelain=v1", "--untracked-files=all").decode().splitlines()
    if args.clean:
        require(not status, "Qualification source is not clean")
    report = {"head": git(root, "rev-parse", "HEAD").decode().strip(), "legacy_blobs": len(files),
              "legacy_guard": "unchanged", "changed_files": changed, "status": status}
    if args.installed:
        canonical = {
            "lux/engine/error/Error.hpp": root / "modules/core/error/include/lux/engine/error/Error.hpp",
            "lux/engine/error/ErrorDescriptor.hpp":
                root / "modules/core/error/include/lux/engine/error/ErrorDescriptor.hpp",
            "lux/engine/description/Image.hpp":
                root / "modules/resource/description/include/lux/engine/description/Image.hpp"
        }
        for header in (render / "core/include").rglob("*.hpp"):
            canonical[header.relative_to(render / "core/include").as_posix()] = header
        installed = args.installed / "include"
        actual = {p.relative_to(installed).as_posix() for p in installed.rglob("*") if p.is_file()}
        require(actual == set(canonical), "Installed header set is incomplete or polluted")
        for name, source in canonical.items():
            require((installed / name).read_bytes() == source.read_bytes(), "Installed header differs: " + name)
        report["installed_headers"] = sorted(actual)
    if args.user_root or args.snapshot:
        require(args.user_root and args.snapshot, "Supply both user-root and snapshot")
        settings = (args.user_root / ".vscode/settings.json").read_bytes()
        require(settings == (args.snapshot / "settings.json").read_bytes(), "User settings changed")
        patch = git(args.user_root, "diff", "--binary", "HEAD", "--", ".vscode/settings.json")
        require(patch == (args.snapshot / "user-settings.patch").read_bytes(), "User patch changed")
        report["user_settings_sha256"] = hashlib.sha256(settings).hexdigest()
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
