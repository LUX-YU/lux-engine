"""Read-only F6.a source/build/codegen closure qualification."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

BASE = "bb3b665e9f3167f14c3aa172bb394fa384fa16e1"
p = argparse.ArgumentParser()
for name in ("source", "build", "output", "baseline-tests"):
    p.add_argument("--" + name, type=Path, required=True)
a = p.parse_args()
src, build, out = a.source.resolve(), a.build.resolve(), a.output.resolve()
out.mkdir(parents=True, exist_ok=True)


def check(value, message):
    if not value:
        raise RuntimeError(message)


def git(*args):
    return subprocess.check_output(["git", "-C", str(src), *args])


def norm(path):
    return str(Path(path).resolve()).replace("\\", "/").lower()


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


check(not git("status", "--porcelain", "-uall"), "dirty qualification source")
check(not build.is_relative_to(src), "build inside source")
changed = [v for v in git("diff", "--name-only", "-z", BASE, "HEAD").decode().split("\0") if v]
allowed = ("modules/function/render/data/", "docs/render-v2/F6", "cmake/render-v2-bootstrap/")
exact = {
    "modules/function/render/graph/cmake/PassSchema.py",
    "modules/function/render/graph/cmake/RenderGraphCodegen.cmake",
    "modules/resource/description/include/lux/engine/description/DataLayout.hpp",
    "engine/toolchain/shader/cmake/GpuValues.py",
    "engine/toolchain/shader/include/lux/engine/toolchain/shader/DataValidation.hpp",
    "engine/toolchain/shader/src/DataValidation.cpp",
    "engine/toolchain/shader/CMakeLists.txt",
}
check(all(v.startswith(allowed) or v in exact for v in changed), "change scope")
protected = ["render_legacy", "modules/function/render/" + "core"]
protected += ["modules/function/render/" + v for v in ("transport", "vulkan", "runtime")]
protected += [
    v for v in git("ls-tree", "-rz", "--name-only", BASE, "--", "docs/render-v2").decode().split("\0") if v
]
protected += ["modules/function/render/graph/" + v for v in ("include", "src", "test")]
objects = {}
for path in protected:
    before = git("rev-parse", BASE + ":" + path).decode().strip()
    after = git("rev-parse", "HEAD:" + path).decode().strip()
    check(before == after, "protected object " + path)
    objects[path] = after
legacy = {}
for row in git("ls-tree", "-rlz", "HEAD", "--", "render_legacy").split(b"\0"):
    if row:
        info, path = row.split(b"\t")
        mode, kind, blob, size = info.decode().split()
        legacy[path.decode()] = (mode, blob, int(size))
manifest = json.loads((src / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text())
check(len(manifest["files"]) == 719 and len(legacy) == 720, "legacy count")
for row in manifest["files"]:
    check(legacy[row["destination"]] == (row["mode"], row["blob"], row["bytes"]), "legacy " + row["source"])
tracked = {norm(src / x) for x in git("ls-files", "-z").decode().split("\0") if x}
forbidden = ("/render_legacy/", "/editor_legacy/", "/engine/scene/", "/engine/editor/", "/render/features/")


def source_input(path):
    check(not any(x in path for x in forbidden), "forbidden input " + path)
    if path.startswith(norm(src) + "/"):
        check(path in tracked, "untracked input " + path)
    check("/install/debug/" not in path and "/install/relwithdebinfo/" not in path, "old SDK input " + path)


reply = build / ".cmake/api/v1/reply"
index = json.loads(max(reply.glob("index-*.json")).read_text())
model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
targets = {}
for ref in model["configurations"][0]["targets"]:
    item = json.loads((reply / ref["jsonFile"]).read_text())
    targets[item["id"]] = item
names = {v["name"]: v for v in targets.values()}
check(names["render_data"]["type"] == "STATIC_LIBRARY", "real Data library")
check(
    {targets[d["id"]]["name"] for d in names["render_data"].get("dependencies", [])} == {"render_core"},
    "Data target dependency",
)
for name in (
    "render_data_header_Schema",
    "render_data_header_Annotations",
    "render_data_header_DataLayout",
    "toolchain_shader_header_DataValidation",
):
    check(name in names, "public header TU " + name)
for part in ("core", "transport", "graph", "vulkan"):
    deps = {targets[d["id"]]["name"] for d in names["render_" + part].get("dependencies", [])}
    check(deps == (set() if part == "core" else {"render_core"}), "preserved layer " + part)
links = {}
for name, item in names.items():
    libraries = [x["fragment"] for x in item.get("link", {}).get("commandFragments", []) if x["role"] == "libraries"]
    links[name] = libraries
    if name in ("render_core", "render_transport", "render_graph", "render_data", "render_runtime"):
        check(
            not any("spirv" in x.lower() or "shader_compiler" in x.lower() for x in libraries),
            "cold tool in runtime link " + name,
        )
commands = json.loads((build / "compile_commands.json").read_text())
for entry in commands:
    source_input(norm(entry["file"]))
    check("std:c++20" in entry["command"] or "std=c++20" in entry["command"], "C++20 " + entry["file"])
deps = subprocess.check_output(["D:/Softwares/ninja-win/ninja.exe", "-C", str(build), "-t", "deps"], text=True)
(out / "ninja-dependencies.txt").write_text(deps, encoding="utf-8")
component = None
headers = {x: set() for x in ("data", "core", "graph", "transport", "vulkan", "runtime")}
for line in deps.splitlines():
    if line and not line.startswith(" "):
        entry = line.replace("\\", "/")
        component = next(
            (x for x in headers if entry.startswith("render_" + x + "/CMakeFiles/render_" + x + ".dir/")), None
        )
    elif line.startswith("    "):
        path = Path(line.strip())
        value = norm(path if path.is_absolute() else build / path)
        source_input(value)
        if component:
            headers[component].add(value)
for name, values in headers.items():
    check(values, "actual compiler dependencies missing " + name)
    if name in ("data", "core", "graph", "transport"):
        check(
            not any("vulkan" in x or "spirv" in x or "/render/runtime/" in x for x in values),
            "neutral native dependency " + name,
        )
    if name == "data":
        check(
            not any(
                "/render/graph/" in x
                or "/render/transport/" in x
                or "/engine/toolchain/" in x
                or "/script/" in x
                or "/math/" in x
                for x in values
            ),
            "Data upward dependency",
        )
    if name == "vulkan":
        check(not any("/render/graph/" in x or "/render/runtime/" in x for x in values), "Foundation upward dependency")
cmake_files = json.loads((reply / index["reply"]["cmakeFiles-v1"]["jsonFile"]).read_text())
for entry in cmake_files["inputs"]:
    path = Path(entry["path"])
    source_input(norm(path if path.is_absolute() else src / "cmake/render-v2-bootstrap" / path))
jobs = []
for path in build.glob("**/lux_codegen/*.json"):
    job = json.loads(path.read_text())
    if "target_files" not in job:
        continue
    check(not job.get("dry_run"), "dry-run codegen")
    for item in job["target_files"]:
        source_input(norm(item["physical_path"]))
    for item in job["projections"]:
        source_input(norm(item["template_path"]))
    jobs.append({"path": str(path), "sha256": digest(path), "job": job})
check(any(x["job"]["marker"] == "luxdata" for x in jobs), "real Data parser job missing")
current = json.loads(
    subprocess.check_output(
        ["D:/Development/CMake/bin/ctest.exe", "--test-dir", str(build), "--show-only=json-v1"], text=True
    )
)
old = json.loads(a.baseline_tests.read_text())
old_names = {x["name"] for x in old["tests"]}
new_names = {x["name"] for x in current["tests"]}
check(len(old_names) == 175 and old_names <= new_names, "lost original test obligations")
artifacts = {}
for pattern in ("*.schema.json", "*.data.hpp", "*.lglslh", "*.spv"):
    for path in build.rglob(pattern):
        artifacts[str(path.relative_to(build))] = digest(path)
result = {
    "implementation": git("rev-parse", "HEAD").decode().strip(),
    "changed": changed,
    "protected_objects": objects,
    "legacy": 719,
    "targets": links,
    "headers": {k: sorted(v) for k, v in headers.items()},
    "codegen": jobs,
    "old_tests": sorted(old_names),
    "new_tests": sorted(new_names - old_names),
    "artifacts": artifacts,
    "status": "PASS",
}
(out / "closure.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
print("F6.a actual dependency/qualification closure PASS; tests", len(new_names))
