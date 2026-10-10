"""Inspect a clean committed R4 build's actual source/include/link/codegen closure."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

BASE = "f9b943c9e768ef2c9d408962f0d5a32d2cf3de74"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def norm(path):
    return str(Path(path).resolve()).replace("\\", "/").lower()


def main():
    parser = argparse.ArgumentParser()
    for name in ("source", "build", "dependencies", "vulkan_include", "vulkan_library", "vma_include", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--ninja", required=True)
    args = parser.parse_args()
    source, build, generic = (p.resolve() for p in (args.source, args.build, args.dependencies))

    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(source), *arguments])

    require(not build.is_relative_to(source), "in-source build")
    require(not args.output.resolve().is_relative_to(source), "in-source evidence")
    require(not git("status", "--porcelain", "--untracked-files=all"), "not a clean snapshot")
    changed = git("diff", "--name-only", BASE, "HEAD").decode().splitlines()
    require(all(p.startswith(("modules/function/render/vulkan/", "docs/render-v2/R4_",
                              "cmake/render-v2-bootstrap/")) for p in changed), "unauthorized change")
    require(not (generic / "include/lux/engine").exists(), "generic dependencies contain Engine SDK")
    tree = {}
    for entry in git("ls-tree", "-rlz", "HEAD", "--", "render_legacy").split(b"\0"):
        if entry:
            info, path = entry.split(b"\t", 1)
            mode, kind, blob, size = info.decode().split()
            tree[path.decode()] = (mode, blob, int(size))
    manifest = json.loads((source / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text())
    for item in manifest["files"]:
        require(tree[item["destination"]] == (item["mode"], item["blob"], item["bytes"]), item["source"])
    require(len(manifest["files"]) == 719 and len(tree) == 720, "frozen file count")
    frozen = ["modules/function/render/" + p for p in ("core", "transport", "graph")]
    frozen += ["render_legacy"]
    frozen += [p for p in git("ls-tree", "-r", "--name-only", BASE, "--", "docs/render-v2").decode().splitlines()]
    for path in frozen:
        require(git("rev-parse", BASE + ":" + path) == git("rev-parse", "HEAD:" + path), "frozen object: " + path)

    reply = build / ".cmake/api/v1/reply"
    index = json.loads(max(reply.glob("index-*.json")).read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    targets = {}
    for ref in model["configurations"][0]["targets"]:
        target = json.loads((reply / ref["jsonFile"]).read_text())
        targets[target["id"]] = target
    by_name = {t["name"]: t for t in targets.values()}
    for part in ("core", "transport", "graph", "vulkan"):
        target = by_name["render_" + part]
        require(target["type"] == "STATIC_LIBRARY", "missing production library: " + part)
        names = {targets[d["id"]]["name"] for d in target.get("dependencies", [])}
        require(names == (set() if part == "core" else {"render_core"}), "component link escape: " + part)
    expected_sources = {"Error.cpp", "Device.cpp", "Vma.cpp", "Memory.cpp", "Descriptors.cpp",
                        "Objects.cpp", "Pipeline.cpp", "Submission.cpp", "Transfer.cpp", "Retirement.cpp"}
    require({Path(s["path"]).name for s in by_name["render_vulkan"]["sources"]} == expected_sources,
            "Vulkan production source set")
    public = source / "modules/function/render/vulkan/include"
    headers = sorted(p.relative_to(public).as_posix() for p in public.rglob("*.hpp"))
    for header in headers:
        name = Path(header).relative_to("lux/engine/render/vulkan").with_suffix("").as_posix().replace("/", "_")
        require("render_vulkan_header_" + name in by_name, "missing independent header: " + header)
    core_includes = {norm(source / "modules/function/render/core/include"),
                     norm(source / "modules/core/error/include"), norm(generic / "include")}
    includes = {p: core_includes | {norm(source / ("modules/function/render/" + p + "/include"))}
                for p in ("core", "transport", "graph", "vulkan")}
    includes["vulkan"].add(norm(args.vulkan_include))
    transport_test = includes["transport"] | {norm(source / "modules/core/type_info/include"),
        norm(source / "modules/function/render/transport/test"), norm(build / "render_transport/test/generated")}
    system_libs = {"kernel32.lib", "user32.lib", "gdi32.lib", "winspool.lib", "shell32.lib", "ole32.lib",
                   "oleaut32.lib", "uuid.lib", "comdlg32.lib", "advapi32.lib"}
    common_libs = {f"render_{p}/lux_engine_render_{p}.lib" for p in ("core", "transport", "graph")}
    native_libs = {"render_vulkan/lux_engine_render_vulkan.lib", "render_vulkan/test/render_vulkan_fault.lib",
                   norm(args.vulkan_library)}
    links = {}
    dashboard = r"(Continuous|Experimental|Nightly)(Build|Configure|Coverage|MemCheck|MemoryCheck|Start|Submit|Test|Update)?"
    for name, target in by_name.items():
        is_native = name.startswith("render_vulkan")
        require(name.startswith(tuple("render_" + p for p in includes)) or
                (target["type"] == "UTILITY" and re.fullmatch(dashboard, name)), "unexpected target: " + name)
        for dependency in target.get("dependencies", []):
            other = targets[dependency["id"]]["name"]
            allowed = {"render_core"}
            if is_native:
                allowed |= {"render_vulkan", "render_vulkan_fault", "render_vulkan_shader"}
            elif name.startswith("render_transport"):
                allowed |= {"render_transport", "render_transport_test_operations_generate"}
            elif name.startswith("render_graph"):
                allowed.add("render_graph")
            require(other in allowed, "target dependency escape: " + name + " -> " + other)
        for group in target.get("compileGroups", []):
            actual = {norm(i["path"]) for i in group.get("includes", [])}
            if is_native:
                allowed = includes["vulkan"]
                if name in {"render_vulkan", "render_vulkan_fault"}:
                    allowed = allowed | {norm(public.parent / "pinclude"), norm(args.vma_include)}
            elif name.startswith("render_graph"):
                allowed = includes["graph"]
            elif name.startswith("render_core"):
                allowed = core_includes
            else:
                allowed = includes["transport"] if name == "render_transport" or "_header_" in name else transport_test
            require(actual == allowed, "include escape: " + name + " " + str(actual ^ allowed))
            definitions = {d["define"] for d in group.get("defines", [])}
            require(("LUX_VULKAN_TEST_SEAM" in definitions) == (name == "render_vulkan_fault"), "native seam escape")
            require(("LUX_RENDER_REPLY_TEST_HOOK" in definitions) == (name == "render_transport_reply_lifetime"),
                    "reply hook escape")
        libraries = []
        for fragment in target.get("link", {}).get("commandFragments", []):
            if fragment["role"] == "libraries":
                libraries.extend(fragment["fragment"].lower().replace("\\", "/").replace('"', '').split())
        require(set(libraries) <= system_libs | common_libs | (native_libs if is_native else set()),
                "unexpected raw link: " + name)
        if is_native:
            require(not any("render_graph" in p or "render_transport" in p for p in libraries), "native upward link")
        links[name] = libraries

    tracked = {norm(source / p) for p in git("ls-files", "-z").decode().split("\0") if p}
    forbidden = ("/render_legacy/", "/editor_legacy/", "/engine/scene/", "/engine/editor/",
                 "/engine/render/runtime/", "/engine/render/features/")

    def check_input(path):
        require(not any(p in path for p in forbidden), "forbidden actual input: " + path)
        if path.startswith(norm(source) + "/"):
            require(path in tracked, "untracked input: " + path)

    deps = subprocess.check_output([args.ninja, "-C", str(build), "-t", "deps"], text=True)
    (args.output.parent / "ninja-dependencies.txt").write_text(deps, encoding="utf-8")
    used = set()
    per_component = {p: set() for p in includes}
    part = None
    for line in deps.splitlines():
        if line and not line.startswith(" "):
            part = next((p for p in includes if line.replace("\\", "/").startswith("render_" + p + "/")), None)
        elif line.startswith("    "):
            path = Path(line.strip())
            path = norm(path if path.is_absolute() else build / path)
            check_input(path)
            used.add(path)
            if part:
                per_component[part].add(path)
    require(all(per_component.values()), "empty native/compiler evidence")
    for part, paths in per_component.items():
        roots = [source / "modules/core/error/include", source / "modules/function/render/core/include"]
        roots += [source / ("modules/function/render/" + part + "/include")]
        if part == "transport":
            roots += [source / "modules/core/type_info/include"]
        require(all(p.startswith(tuple(norm(r) + "/" for r in roots))
                    for p in paths if "/lux/engine/" in p), "actual Engine header escape: " + part)
        if part != "vulkan":
            require(not any("vulkan" in p or "vk_mem_alloc" in p for p in paths), "native header in " + part)
        else:
            require(not any("/lux/cxx/concurrent/" in p for p in paths), "native concurrency dependency")
    sources = [norm(e["file"]) for e in json.loads((build / "compile_commands.json").read_text())]
    for path in sources:
        check_input(path)
        require(path.startswith(tuple(norm(source / ("modules/function/render/" + p)) + "/" for p in includes)) or
                (path.startswith(norm(build) + "/") and re.fullmatch(r"header_\w+\.cpp", Path(path).name)),
                "unexpected compile source: " + path)
    cmake_files = json.loads((reply / index["reply"]["cmakeFiles-v1"]["jsonFile"]).read_text())
    cmake_inputs = []
    for entry in cmake_files["inputs"]:
        p = Path(entry["path"])
        path = norm(p if p.is_absolute() else source / "cmake/render-v2-bootstrap" / p)
        check_input(path)
        cmake_inputs.append(path)
    job_root = build / "render_transport/test/lux_codegen"
    job = json.loads((job_root / "render_transport_test_operations.json").read_text())
    require(job["marker"] == "luxop" and not job["dry_run"], "missing real codegen")
    require([norm(p["physical_path"]) for p in job["target_files"]] ==
            [norm(source / "modules/function/render/transport/test/TestOps.hpp")], "codegen source escape")
    require(len(job["projections"]) == 1 and norm(job["projections"][0]["template_path"]) ==
            norm(source / "modules/function/render/transport/cmake/transport_ops.template"), "codegen template escape")
    codegen = []
    for line in (job_root / "render_transport_test_operations.d").read_text().splitlines()[1:]:
        value = line.strip().removesuffix("\\").strip()
        if value:
            path = norm(re.sub(r"\\(.)", r"\1", value))
            check_input(path)
            require("vulkan" not in path, "Vulkan escaped into transport codegen")
            codegen.append(path)
    require(codegen, "missing codegen dependencies")
    generated = build / "render_transport/test/generated/TestOps.ops.hpp"
    content = generated.read_text(encoding="utf-8")
    require(content.count("struct RenderOpTraits<") == 6 and content.count("struct RenderReplyTraits<") == 1 and
            content.count("static_assert(PacketValue<") == 7, "silently incomplete operation traits")
    ninja = (build / "build.ninja").read_text()
    require("render_legacy" not in ninja.lower() and "glslc" in ninja and "Smoke.comp" in ninja, "shader/input closure")
    for p in (source / "modules/function/render/vulkan").rglob("*"):
        if p.suffix in {".cpp", ".hpp"} and "/test/" not in p.as_posix():
            code = re.sub(r"//[^\n]*", "", p.read_text(encoding="utf-8"))
            require(not re.search(r"\b(throw|try|catch)\b|\b(assert|printf|fprintf|vkDeviceWaitIdle)\s*\(", code),
                    "production error/lifetime policy: " + str(p))
    report = {"status": "PASS", "implementation": git("rev-parse", "HEAD").decode().strip(), "base": BASE,
        "changed_files": changed, "frozen_objects": frozen, "legacy_files_verified": 719,
        "public_headers": headers, "target_names": sorted(by_name), "link_libraries": links,
        "compile_sources": sources, "cmake_inputs": cmake_inputs, "codegen_inputs": codegen,
        "compiler_headers": {p: sorted(paths) for p, paths in per_component.items()},
        "generated_traits_sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
        "v2_product": "EXPECTED_UNAVAILABLE"}
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS closure", len(used), "headers;", len(sources), "compile entries;", len(changed), "changed files")


if __name__ == "__main__":
    main()
