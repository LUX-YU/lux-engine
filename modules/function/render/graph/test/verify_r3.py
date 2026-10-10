"""Qualify committed R3 inputs against the actual CMake and compiler closure."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


BASE = "e0eabe1640d94fafb80a57bd334507162f3b2f9f"


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
    allowed = ("modules/function/render/graph/", "docs/render-v2/R3_")
    require(all(p.startswith(allowed) or p == "cmake/render-v2-bootstrap/CMakeLists.txt" for p in changed),
            "change outside authorized R3 paths")
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
    graph = by_name["render_graph"]
    require(graph["type"] == "STATIC_LIBRARY", "missing real graph library")
    require([targets[d["id"]]["name"] for d in graph.get("dependencies", [])] == ["render_core"],
            "Graph compiled dependency escape")
    require({Path(s["path"]).name for s in graph["sources"]} ==
            {"Definition.cpp", "Plan.cpp", "Bindings.cpp"}, "Graph source escape")
    for name in ("Definition", "Plan", "Bindings"):
        require("render_graph_header_" + name in by_name, "missing Graph independent header TU")
    core_includes = {norm(source / "modules/function/render/core/include"),
                     norm(source / "modules/core/error/include"), norm(dependencies / "include")}
    graph_includes = core_includes | {norm(source / "modules/function/render/graph/include")}
    transport_includes = core_includes | {norm(source / "modules/function/render/transport/include")}
    test_includes = transport_includes | {norm(source / "modules/function/render/transport/test"),
                                         norm(source / "modules/core/type_info/include"),
                                         norm(build / "render_transport/test/generated")}
    dashboard = r"(Continuous|Experimental|Nightly)(Build|Configure|Coverage|MemCheck|MemoryCheck|Start|Submit|Test|Update)?"
    system_libs = {"kernel32.lib", "user32.lib", "gdi32.lib", "winspool.lib", "shell32.lib", "ole32.lib",
                   "oleaut32.lib", "uuid.lib", "comdlg32.lib", "advapi32.lib"}
    component_libs = {"render_core/lux_engine_render_core.lib", "render_transport/lux_engine_render_transport.lib",
                      "render_graph/lux_engine_render_graph.lib"}
    for target in targets.values():
        name = target["name"]
        require(name.startswith(("render_core", "render_transport", "render_graph")) or
                (target["type"] == "UTILITY" and re.fullmatch(dashboard, name)), "unexpected target: " + name)
        for dep in target.get("dependencies", []):
            other = targets[dep["id"]]["name"]
            require(other in {"render_core", "render_transport", "render_graph"} or other.endswith("_operations_generate"),
                    "compiled dependency escape: " + other)
        for group in target.get("compileGroups", []):
            paths = {norm(i["path"]) for i in group.get("includes", [])}
            if name.startswith("render_core"):
                require(paths == core_includes, "Core include escape")
            elif name.startswith("render_graph"):
                require(paths == graph_includes, "Graph include escape")
            elif name == "render_transport" or name.startswith("render_transport_header_"):
                require(paths == transport_includes, "Transport public include escape")
            else:
                require(paths == test_includes, "test include escape: " + name)
        if name.startswith("render_graph"):
            require(all(targets[d["id"]]["name"] in {"render_core", "render_graph"}
                        for d in target.get("dependencies", [])), "Graph links a forbidden component")
        for fragment in target.get("link", {}).get("commandFragments", []):
            if fragment["role"] == "libraries":
                libraries = fragment["fragment"].lower().replace("\\", "/").split()
                require(set(libraries) <= system_libs | component_libs, "unexpected linked library")
                if name.startswith("render_graph"):
                    require(not any("transport" in lib for lib in libraries), "Graph raw Transport link")
    for target in targets.values():
        definitions = {d["define"] for group in target.get("compileGroups", [])
                       for d in group.get("defines", [])}
        has_hook = "LUX_RENDER_REPLY_TEST_HOOK" in definitions
        require(has_hook == (target["name"] == "render_transport_reply_lifetime"),
                "test hook escaped its instrumented executable")
    require(not git("diff", "--name-only", BASE, "HEAD", "--",
                    "modules/function/render/core", "modules/function/render/transport", "render_legacy",
                    "docs/render-v2/R2_VERIFICATION.md",
                    "modules/function/render/transport/test/Contracts.cpp",
                    "modules/function/render/transport/test/Concurrency.cpp",
                    "modules/function/render/transport/test/Benchmark.cpp",
                    "modules/function/render/transport/cmake"), "frozen reference/test/benchmark changed")
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
                 "/engine/render/runtime/", "/engine/render/features/")
    require(not any(token in path for token in forbidden for path in used), "forbidden compiled header")
    roots = tuple(norm(source / p) + "/" for p in (
        "modules/function/render/core/include", "modules/function/render/transport/include",
        "modules/core/error/include", "modules/core/type_info/include", "modules/function/render/graph/include"))
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

    compile_sources = []
    for entry in json.loads((build / "compile_commands.json").read_text()):
        path = norm(entry["file"])
        compile_sources.append(path)
        require(not any(token in path for token in forbidden), "forbidden compilation source")
        if path.startswith(norm(source) + "/"):
            require(path in tracked, "untracked compilation source: " + path)
            require(path.startswith(tuple(norm(source / ("modules/function/render/" + part)) + "/"
                                          for part in ("core", "transport", "graph"))), "source escapes Render")
        else:
            require(path.startswith(norm(build) + "/") and
                    re.fullmatch(r"header_\w+\.cpp", Path(path).name), "unexpected generated compilation source")
    cmake_files = json.loads((reply / index["reply"]["cmakeFiles-v1"]["jsonFile"]).read_text())
    cmake_inputs = []
    for entry in cmake_files["inputs"]:
        path = Path(entry["path"])
        # CMake relative input paths are relative to the top-level CMake source.
        path = norm(path if path.is_absolute() else source / "cmake/render-v2-bootstrap" / path)
        cmake_inputs.append(path)
        require(not any(token in path for token in forbidden), "forbidden CMake input")
        if path.startswith(norm(source) + "/"):
            require(path in tracked, "untracked CMake input: " + path)

    graph_headers = set()
    graph_object = False
    for line in deps.splitlines():
        if line and not line.startswith(" "):
            graph_object = line.replace("\\", "/").startswith("render_graph/")
        elif graph_object and line.startswith("    "):
            path = Path(line.strip())
            graph_headers.add(norm(path if path.is_absolute() else build / path))
    require(graph_headers, "missing Graph compiler dependencies")
    graph_roots = tuple(norm(source / p) + "/" for p in (
        "modules/function/render/core/include", "modules/function/render/graph/include", "modules/core/error/include"))
    require(all(p.startswith(graph_roots) for p in graph_headers if "/lux/engine/" in p),
            "Graph compiler inputs escape its allowed closure")
    require(not any("/lux/cxx/concurrent/" in p for p in graph_headers), "Graph concurrency dependency")

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
    for path in (source / "modules/function/render/graph").rglob("*"):
        if path.suffix not in {".cpp", ".hpp"} or "/test/" in path.as_posix():
            continue
        code = re.sub(r"//[^\n]*", "", path.read_text(encoding="utf-8"))
        require(not re.search(r"\b(throw|try|catch)\b|\b(assert|printf|fprintf)\s*\(", code),
                "production exception/terminal policy: " + str(path))
    report = {
        "status": "PASS", "implementation": git("rev-parse", "HEAD").decode().strip(), "base": BASE,
        "changed_files": changed, "legacy_files_verified": 719, "core_unchanged": True, "transport_unchanged": True,
        "targets": sorted(by_name), "engine_headers": engine_headers, "compiler_header_count": len(used),
        "transport_includes": sorted(transport_includes), "graph_includes": sorted(graph_includes),
        "codegen_inputs": codegen_inputs, "compile_sources": compile_sources, "cmake_inputs": cmake_inputs,
        "graph_compiler_headers": sorted(graph_headers),
        "generated_traits_sha256":
        hashlib.sha256(generated.read_bytes()).hexdigest(), "v2_product": "EXPECTED_UNAVAILABLE"
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
