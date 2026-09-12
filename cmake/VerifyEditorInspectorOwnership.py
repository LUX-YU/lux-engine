"""Inspect actual compile/link provenance of generated Inspector objects in a qualified MSVC build."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--build", required=True)
parser.add_argument("--pdbutil", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--commit", required=True)
args = parser.parse_args()
build, output = Path(args.build), Path(args.output)
output.mkdir(parents=True, exist_ok=True)
commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8-sig"))
compiled = []
for entry in commands:
    if not entry["file"].endswith(".inspector.generated.cpp"):
        continue
    assert "/engine/editor/" in entry["output"].replace("\\", "/"), entry
    if "editor_scene_ui.dir" in entry["output"]:
        assert "LUX_EDITOR_INSPECTOR_TEST_PROBE" not in entry["command"]
    compiled.append(dict(source=entry["file"], object=entry["output"],
                         sha256=hashlib.sha256(Path(entry["file"]).read_bytes()).hexdigest()))
assert len(compiled) == 8, compiled  # five first-party components, two test components, one cost component
records = []
for dll in sorted((build / "bin").glob("*.dll")):
    pdb = dll.with_suffix(".pdb")
    if not pdb.exists():
        continue  # External prebuilt dependencies have no project link PDB.
    result = subprocess.run([args.pdbutil, "dump", "--modules", str(pdb)], capture_output=True, check=True)
    (output / (dll.stem + ".pdb-modules.txt")).write_bytes(result.stdout)
    modules = result.stdout.decode("utf-8", errors="replace")
    generated = [line for line in modules.splitlines() if line.startswith("Mod ") and
                 ".inspector.generated.cpp.obj" in line]
    if dll.name == "lux_engine_editor_scene_ui.dll":
        assert len(generated) == 5, generated
    else:
        assert not generated, (dll, generated)
    records.append(dict(dll=dll.name, sha256=hashlib.sha256(dll.read_bytes()).hexdigest(),
                        pdb_sha256=hashlib.sha256(pdb.read_bytes()).hexdigest(), generated_objects=generated))
assert any(r["dll"] == "ui.dll" for r in records)
assert any(r["dll"] == "lux_engine_editor_scene_ui.dll" for r in records)
(output / "ownership.json").write_text(json.dumps(dict(source_commit=args.commit,
    configuration="RelWithDebInfo", compiled=compiled, linked=records), indent=2), encoding="utf-8")
print("Generated Inspector compile ownership and actual DLL linker PDB closure PASS:", len(records), "DLLs")
