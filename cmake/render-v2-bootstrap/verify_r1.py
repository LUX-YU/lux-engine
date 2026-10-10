"""Audit a committed R1 clone and its actual Ninja/CMake build closure."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


BASE = "db2c42c3d98277c71e63c1b102cfab57d2b05f29"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    parser.add_argument("--ninja", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source, build, dependencies = (p.resolve() for p in (args.source, args.build, args.dependencies))

    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(source), *arguments])

    def require(condition, message):
        if not condition:
            raise RuntimeError(message)

    def normalized(path):
        return str(Path(path).resolve()).replace("\\", "/").lower()

    require(not build.is_relative_to(source), "build must be outside source")
    require(not args.output.resolve().is_relative_to(source), "evidence must be outside source")
    require(not git("status", "--porcelain", "--untracked-files=all"), "source is not clean")
    changed = git("diff", "--name-only", BASE, "HEAD").decode().splitlines()
    allowed = ("modules/function/render/core/", "cmake/render-v2-bootstrap/", "docs/render-v2/R1_")
    require(all(name.startswith(allowed) for name in changed), "changes exceed R1 scope")
    require(not git("diff", "--name-only", BASE, "HEAD", "--", "render_legacy"), "legacy changed")
    manifest = json.loads((source / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text())
    tree = {}
    for entry in git("ls-tree", "-rlz", "HEAD", "--", "render_legacy").split(b"\0"):
        if entry:
            info, name = entry.split(b"\t", 1)
            mode, kind, blob, size = info.decode().split()
            tree[name.decode()] = (mode, blob, int(size))
    for item in manifest["files"]:
        require(tree[item["destination"]] == (item["mode"], item["blob"], item["bytes"]), item["source"])
    require(len(tree) == 720, "legacy count changed")

    reply = build / ".cmake/api/v1/reply"
    index = json.loads(max(reply.glob("index-*.json")).read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    targets = {}
    for ref in model["configurations"][0]["targets"]:
        target = json.loads((reply / ref["jsonFile"]).read_text())
        targets[target["id"]] = target
    cores = [target for target in targets.values() if target["name"] == "render_core"]
    require(len(cores) == 1 and cores[0]["type"] == "STATIC_LIBRARY", "missing real static Core")
    dashboard = r"(Continuous|Experimental|Nightly)(Build|Configure|Coverage|MemCheck|MemoryCheck|Start|Submit|Test|Update)?"
    require(all(t["name"].startswith("render_core") or
                (t["type"] == "UTILITY" and re.fullmatch(dashboard, t["name"]))
                for t in targets.values()), "unexpected configured target")
    core = cores[0]
    expected_includes = {
        normalized(source / "modules/function/render/core/include"),
        normalized(source / "modules/core/error/include"),
        normalized(dependencies / "include")
    }
    require(not (dependencies / "include/lux/engine").exists(), "dependency prefix contains an engine SDK")
    includes = {normalized(i["path"]) for group in core["compileGroups"] for i in group.get("includes", [])}
    require(includes == expected_includes, "unexpected Core include closure")
    require(not core.get("dependencies"), "Core has a compiled runtime dependency")
    require(len(core["sources"]) == 1, "unexpected Core source count")
    require(core["sources"][0]["path"].endswith("core/src/Descriptors.cpp"), "unexpected Core source")
    for target in targets.values():
        for dep in target.get("dependencies", []):
            require(targets[dep["id"]]["name"] == "render_core", "unexpected compiled dependency")
        for group in target.get("compileGroups", []):
            paths = {normalized(i["path"]) for i in group.get("includes", [])}
            require(paths == expected_includes, "consumer include escape")
        for fragment in target.get("link", {}).get("commandFragments", []):
            if fragment["role"] == "libraries":
                libraries = fragment["fragment"].lower().split()
                system = {"kernel32.lib", "user32.lib", "gdi32.lib", "winspool.lib", "shell32.lib",
                          "ole32.lib", "oleaut32.lib", "uuid.lib", "comdlg32.lib", "advapi32.lib"}
                require(all(lib in system or lib.replace("\\", "/") ==
                            "render_core/lux_engine_render_core.lib" for lib in libraries), "link escape")

    deps = subprocess.check_output([args.ninja, "-C", str(build), "-t", "deps"], text=True)
    (args.output.parent / "ninja-dependencies.txt").write_text(deps, encoding="utf-8")
    used_headers = set()
    for line in deps.splitlines():
        if line.startswith("    "):
            path = Path(line.strip())
            path = path if path.is_absolute() else build / path
            used_headers.add(normalized(path))
    require(used_headers, "compiler dependency database is empty")
    forbidden = ("/render_legacy/", "/editor_legacy/", "/vulkan/", "/engine/scene/", "/engine/editor/",
                 "/engine/render/transport/", "/engine/render/runtime/", "/engine/render/features/",
                 "/lux/cxx/concurrent/", "/lux/cxx/concurrency/")
    require(not any(token in path for path in used_headers for token in forbidden), "forbidden compiled header")
    require(not any(Path(path).name in {"thread", "mutex", "shared_mutex", "condition_variable",
                                        "errorregistry.hpp"} for path in used_headers), "thread/service header")
    engine_headers = [path for path in used_headers if "/lux/engine/" in path]
    allowed_header_roots = [normalized(source / "modules/function/render/core/include") + "/",
                            normalized(source / "modules/core/error/include") + "/"]
    require(all(path.startswith(tuple(allowed_header_roots)) for path in engine_headers), "external engine header")
    error_headers = [path for path in engine_headers if "/core/error/" in path]
    require({Path(path).name for path in error_headers} == {"error.hpp", "errordescriptor.hpp"}, "error runtime leak")

    source_files = list((source / "modules/function/render/core/src").rglob("*.cpp"))
    for path in source_files:
        content = path.read_text(encoding="utf-8")
        require(not re.search(r"\b(throw|try|catch)\b|\b(assert|printf|fprintf)\s*\(", content), "hot-path policy")
    for header in ("Identity", "Handles", "Error", "Descriptors", "Target"):
        require(any(t["name"] == "render_core_header_" + header for t in targets.values()), "missing header TU")
    require(not any("install" in p.name.lower() for p in build.glob("install_manifest*")), "unexpected installation")
    report = {
        "status": "PASS", "implementation": git("rev-parse", "HEAD").decode().strip(), "base": BASE,
        "changed_files": changed, "legacy_files_verified": 719, "core_type": core["type"],
        "targets": sorted(t["name"] for t in targets.values()), "core_includes": sorted(includes),
        "engine_headers": sorted(engine_headers), "compiler_header_count": len(used_headers),
        "ninja_dependencies_sha256": hashlib.sha256(deps.encode()).hexdigest(),
        "v2_product": "EXPECTED_UNAVAILABLE", "next_stage": "R2 / render_transport", "action": "STOP"
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
