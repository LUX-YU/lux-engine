"""Read-only R3-A boundary/provenance checks; GPU behavior is tested separately."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

BASE = "960e755395be2c9f78a86339a062c507826c4eb4"
FROZEN = "a669409a289a6fa4092f21176397795b1cdb7f3e"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--implementation", required=True)
    parser.add_argument("--design-zip", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.source.resolve()
    output = args.output.resolve()
    if output.is_relative_to(root):
        raise SystemExit("Evidence must be outside source")
    output.mkdir(parents=True, exist_ok=True)
    checks = []

    def check(name, value):
        checks.append({"name": name, "passed": bool(value)})

    def git(*values):
        return subprocess.check_output(["git", "-C", str(root), *values])

    def tree(ref):
        result = {}
        for record in git("ls-tree", "-rlz", ref).split(b"\0"):
            if record:
                meta, name = record.split(b"\t")
                mode, _, blob, size = meta.split()
                result[name.decode()] = (mode.decode(), blob.decode(), int(size))
        return result

    check("exact I", git("rev-parse", "HEAD").decode().strip() == args.implementation)
    check("clean", not git("status", "--porcelain", "--untracked-files=all"))
    before, current, frozen = tree(BASE), tree("HEAD"), tree(FROZEN)
    changes = sorted(p for p in before.keys() | current.keys() if before.get(p) != current.get(p))
    native = "modules/function/render/vulkan/"
    allowed_headers = set()
    for name in changes:
        allowed = (name.startswith("modules/function/render/runtime/")
                   or name.startswith("docs/render-v2/rd3r3/")
                   or name == "cmake/render-v2-bootstrap/RD3Runtime.cmake"
                   or name in allowed_headers)
        check("allowed " + name, allowed)
    check("no deleted files", not (before.keys() - current.keys()))
    for name, value in before.items():
        protected = (name.startswith("docs/") or name.startswith("render_legacy/")
                     or name.startswith(native + "src/")
                     or name.startswith("modules/function/render/graph/src/"))
        if protected:
            check("protected " + name, current.get(name) == value)
    manifest = json.loads((root / "docs/render-v2/LEGACY_SOURCE_MANIFEST.json").read_text())
    check("719 frozen sources", len(manifest["files"]) == manifest["file_count"] == 719)
    for entry in manifest["files"]:
        value = (entry["mode"], entry["blob"], entry["bytes"])
        check("frozen " + entry["source"], frozen.get(entry["source"]) == value)
        check("legacy " + entry["destination"], current.get(entry["destination"]) == value)
    for name, (_, blob, _) in current.items():
        data = (root / name).read_bytes()
        digest = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        check("exact checkout " + name, digest == blob)
    import runpy
    package = root / "docs/render-v2/rd3r3/design"
    import zipfile
    check("ZIP SHA256", hashlib.sha256(args.design_zip.read_bytes()).hexdigest() ==
        "af83aeb7ab691e9316748e7434a9176c220fc56a0761592965075925fca68856")
    with zipfile.ZipFile(args.design_zip) as archive:
        check("ZIP CRC", archive.testzip() is None)
        members = [item for item in archive.infolist() if not item.is_dir()]
        check("ZIP file count", len(members) == 65)
        for item in members:
            relative = item.filename.split("/", 1)[1]
            check("ZIP original bytes " + relative, (package / relative).read_bytes() == archive.read(item))
    integrity = runpy.run_path(str(package / "checks/verify_package.py"))["check"](package)
    check("design manifest and all tables", integrity == 450)
    import csv, re
    def rows(name):
        with (package / "appendix" / name).open(encoding="utf-8-sig") as stream:
            return list(csv.DictReader(stream))
    workloads = {r["WorkloadId"] for r in rows("GoldenWorkloads.csv")}
    check("exact W01-W25", workloads == {f"W{i:02}" for i in range(1, 26)})
    check("exact F/I/H IDs", {r["Id"] for r in rows("CapabilityMatrix.csv")} ==
        {f"{prefix}{i:02}" for prefix, count in [("F", 36), ("I", 44), ("H", 6)] for i in range(1, count + 1)})
    check("exact REQ01-REQ51", {r["RequirementId"] for r in rows("RequirementsTrace.csv")} ==
        {f"REQ{i:02}" for i in range(1, 52)})
    for requirement in rows("RequirementsTrace.csv"):
        check("requirement workload FK " + requirement["RequirementId"],
            all(key in workloads for key in re.findall(r"W\d{2}", requirement["RequiredGoldenIds"])))
    source = (root / "modules/function/render/runtime/src/Backend.cpp").read_text()
    hot = source[source.index("RenderResult<void> RenderBackend::Impl::execute("):source.index("void RenderBackend::Impl::run()")]
    check("hot execution has no object lookup", "find(" not in hot and "tryGet(" not in hot and "getOrCreate" not in hot)
    for name in changes:
        if name.endswith((".hpp", ".cpp")) and "/archive/" not in name:
            text = (root / name).read_text()
            check("no new hash object authority " + name, "unordered_map" not in text and "unordered_set" not in text)
    for archived, original in [("P2A_ResourceDomains.hpp", "ResourceDomains.hpp"), ("P2A_RD3Faults.cpp", "RD3Faults.cpp")]:
        directory = "modules/function/render/runtime/test/"
        check("archived superseded test " + original,
            (root / directory / "archive" / archived).read_bytes() == git("show", BASE + ":" + directory + original))
    git_dir = Path(git("rev-parse", "--absolute-git-dir").decode().strip()).resolve()
    common = Path(git("rev-parse", "--git-common-dir").decode().strip())
    common = (common if common.is_absolute() else root / common).resolve()
    check("independent git dir", git_dir == common and common.is_relative_to(root))
    check("no alternates", not (common / "objects/info/alternates").exists())
    objects = [p for p in (common / "objects").rglob("*") if p.is_file()]
    check("no hardlinked objects", bool(objects) and max(p.stat().st_nlink for p in objects) == 1)
    failures = [item["name"] for item in checks if not item["passed"]]
    result = {"implementation": args.implementation, "base": BASE, "status": "FAIL" if failures else "PASS",
              "checks": len(checks), "failures": failures, "changes": changes, "legacy_count": 719}
    (output / "checks.json").write_text(json.dumps(checks, indent=2) + "\n")
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
