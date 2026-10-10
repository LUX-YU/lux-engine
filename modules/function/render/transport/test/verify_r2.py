"""Qualify committed R2 inputs against the actual CMake and compiler closure."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


BASE = "14319331d616dbfe0318352857cf34676df6b564"


def main():
    parser = argparse.ArgumentParser()
    for name in ("source", "build", "dependencies", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--ninja", required=True)
    args = parser.parse_args()
    source, build, dependencies = (p.resolve() for p in (args.source, args.build, args.dependencies))

    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(source), *arguments])

    def require(condition, message):
        if not condition:
            raise RuntimeError(message)

    def norm(path):
        return str(Path(path).resolve()).replace("\\", "/").lower()

    require(not build.is_relative_to(source), "build inside source")
    require(not args.output.resolve().is_relative_to(source), "evidence inside source")
    require(not git("status", "--porcelain", "--untracked-files=all"), "source not clean")
    changed = git("diff", "--name-only", BASE, "HEAD").decode().splitlines()
    allowed = ("modules/function/render/transport/", "docs/render-v2/R2_")
    require(all(p.startswith(allowed) or p == "cmake/render-v2-bootstrap/CMakeLists.txt" for p in changed),
            "change outside authorized R2 paths")
    manifest = json.loads((source / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text())
    tree = {}
    for entry in git("ls-tree", "-rlz", "HEAD", "--", "render_legacy").split(b"\0"):
        if entry:
            info, name = entry.split(b"\t", 1)
            mode, kind, blob, size = info.decode().split()
            tree[name.decode()] = (mode, blob, int(size))
    for item in manifest["files"]:
        require(tree[item["destination"]] == (item["mode"], item["blob"], item["bytes"]), item["source"])
    require(len(tree) == 720, "legacy tree count changed")
    require(not (dependencies / "include/lux/engine").exists(), "generic prefix contains Engine SDK")

    reply = build / ".cmake/api/v1/reply"
    index = json.loads(max(reply.glob("index-*.json")).read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    targets = {}
    for ref in model["configurations"][0]["targets"]:
        target = json.loads((reply / ref["jsonFile"]).read_text())
        targets[target["id"]] = target
    by_name = {t["name"]: t for t in targets.values()}
    core, transport = (by_name[n] for n in ("render_core", "render_transport"))
    require(core["type"] == transport["type"] == "STATIC_LIBRARY", "missing real static components")
    require(not core.get("dependencies"), "Core runtime dependency")
    require([targets[d["id"]]["name"] for d in transport.get("dependencies", [])] == ["render_core"],
            "Transport compiled dependency escape")
    core_includes = {norm(source / "modules/function/render/core/include"),
                     norm(source / "modules/core/error/include"), norm(dependencies / "include")}
    transport_includes = core_includes | {norm(source / "modules/function/render/transport/include")}
    test_includes = transport_includes | {norm(source / "modules/function/render/transport/test"),
                                         norm(source / "modules/core/type_info/include"),
                                         norm(build / "render_transport/test/generated")}
    dashboard = r"(Continuous|Experimental|Nightly)(Build|Configure|Coverage|MemCheck|MemoryCheck|Start|Submit|Test|Update)?"
    system_libs = {"kernel32.lib", "user32.lib", "gdi32.lib", "winspool.lib", "shell32.lib", "ole32.lib",
                   "oleaut32.lib", "uuid.lib", "comdlg32.lib", "advapi32.lib"}
    component_libs = {"render_core/lux_engine_render_core.lib", "render_transport/lux_engine_render_transport.lib"}
    for target in targets.values():
        name = target["name"]
        require(name.startswith(("render_core", "render_transport")) or
                (target["type"] == "UTILITY" and re.fullmatch(dashboard, name)), "unexpected target: " + name)
        for dep in target.get("dependencies", []):
            other = targets[dep["id"]]["name"]
            require(other in {"render_core", "render_transport"} or other.endswith("_operations_generate"),
                    "compiled dependency escape: " + other)
        for group in target.get("compileGroups", []):
            paths = {norm(i["path"]) for i in group.get("includes", [])}
            if name.startswith("render_core"):
                require(paths == core_includes, "Core include escape")
            elif name == "render_transport" or name.startswith("render_transport_header_"):
                require(paths == transport_includes, "Transport public include escape")
            else:
                require(paths == test_includes, "test include escape: " + name)
        for fragment in target.get("link", {}).get("commandFragments", []):
            if fragment["role"] == "libraries":
                libraries = fragment["fragment"].lower().replace("\\", "/").split()
                require(set(libraries) <= system_libs | component_libs, "unexpected linked library")
    require({Path(s["path"]).name for s in transport["sources"]} ==
            {"Error.cpp", "Routes.cpp", "Replies.cpp", "Packet.cpp", "Transport.cpp"}, "production source escape")
    for name in ("Error", "Operation", "Routes", "Replies", "Packet", "Transport"):
        require("render_transport_header_" + name in by_name, "missing independent header TU")

    deps = subprocess.check_output([args.ninja, "-C", str(build), "-t", "deps"], text=True)
    (args.output.parent / "ninja-dependencies.txt").write_text(deps, encoding="utf-8")
    used = set()
    for line in deps.splitlines():
        if line.startswith("    "):
            path = Path(line.strip())
            used.add(norm(path if path.is_absolute() else build / path))
    require(used, "empty compiler dependency database")
    forbidden = ("/render_legacy/", "/editor_legacy/", "/vulkan/", "/engine/scene/", "/engine/editor/",
                 "/engine/render/runtime/", "/engine/render/features/", "/engine/render/graph/")
    require(not any(token in path for token in forbidden for path in used), "forbidden compiled header")
    roots = tuple(norm(source / p) + "/" for p in (
        "modules/function/render/core/include", "modules/function/render/transport/include",
        "modules/core/error/include", "modules/core/type_info/include"))
    engine_headers = sorted(p for p in used if "/lux/engine/" in p)
    require(all(p.startswith(roots) for p in engine_headers), "external Engine SDK")
    require({Path(p).name for p in engine_headers if "/core/error/" in p} ==
            {"error.hpp", "errordescriptor.hpp"}, "error registry dependency")
    require({Path(p).name for p in engine_headers if "/core/type_info/" in p} == {"metaannotations.hpp"},
            "type_info runtime dependency")
    tracked = {norm(source / p) for p in git("ls-files", "-z").decode().split("\0") if p}
    for path in used:
        if path.startswith(norm(source) + "/"):
            require(path in tracked, "untracked compiler input: " + path)

    generated = build / "render_transport/test/generated/TestOps.ops.hpp"
    job_root = build / "render_transport/test/lux_codegen"
    job = json.loads((job_root / "render_transport_test_operations.json").read_text())
    require(job["marker"] == "luxop" and not job["dry_run"], "codegen did not parse luxop")
    require([norm(p["physical_path"]) for p in job["target_files"]] ==
            [norm(source / "modules/function/render/transport/test/TestOps.hpp")], "codegen author input escape")
    require(len(job["projections"]) == 1 and norm(job["projections"][0]["template_path"]) ==
            norm(source / "modules/function/render/transport/cmake/transport_ops.template"), "codegen template escape")
    codegen_inputs = []
    for line in (job_root / "render_transport_test_operations.d").read_text().splitlines()[1:]:
        value = line.strip().removesuffix("\\").strip()
        if not value:
            continue
        path = norm(re.sub(r"\\(.)", r"\1", value))
        codegen_inputs.append(path)
        require(not any(token in path for token in forbidden), "forbidden codegen input")
        if "/lux/engine/" in path:
            require(path.startswith(roots), "old SDK codegen input")
        if path.startswith(norm(source) + "/"):
            require(path in tracked, "untracked codegen input")
    require(codegen_inputs, "empty generator dependency record")
    content = generated.read_text(encoding="utf-8")
    require(content.count("struct RenderOpTraits<") == 6 and content.count("struct RenderReplyTraits<") == 1,
            "silently incomplete generated operation traits")
    require(content.count("static_assert(PacketValue<") == 7, "missing generated ownership checks")
    ninja = (build / "build.ninja").read_text(encoding="utf-8")
    require("lux_meta_generator" in ninja and "transport_ops.template" in ninja, "missing real codegen job")
    require("render_legacy" not in ninja.lower(), "legacy build/codegen input")
    for path in (source / "modules/function/render/transport").rglob("*"):
        if path.suffix not in {".cpp", ".hpp"} or "/test/" in path.as_posix():
            continue
        code = re.sub(r"//[^\n]*", "", path.read_text(encoding="utf-8"))
        require(not re.search(r"\b(throw|try|catch)\b|\b(assert|printf|fprintf)\s*\(", code),
                "production exception/terminal policy: " + str(path))
    report = {
        "status": "PASS", "implementation": git("rev-parse", "HEAD").decode().strip(), "base": BASE,
        "changed_files": changed, "legacy_files_verified": 719, "core_unchanged": True,
        "targets": sorted(by_name), "engine_headers": engine_headers, "compiler_header_count": len(used),
        "transport_includes": sorted(transport_includes), "codegen_inputs": codegen_inputs,
        "generated_traits_sha256":
        hashlib.sha256(generated.read_bytes()).hexdigest(), "v2_product": "EXPECTED_UNAVAILABLE"
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
