"""Build-time contract negatives against real serialized MetaUnit; never execute stale C++ on rejection."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
script, config_path, ir_path = map(Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location("editor_codegen", script)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
config = json.loads(config_path.read_text(encoding="utf-8-sig"))
data = json.loads(ir_path.read_text(encoding="utf-8-sig"))
outputs = module.generate(config, data)
assert len([p for p in outputs if p.endswith(".cpp")]) == len(config["components"])
combined = "\n".join(outputs.values())
for call in ("InputScalar", "DragScalar", "SliderScalar", "Checkbox", "InputText", "BeginCombo"):
    assert "ImGui::" + call in combined, call
assert "InspectorWidget<inspector_fixture::Angle" in combined
assert "ReflectionRegistry" not in combined and "RefField" not in combined
assert "catch (" not in combined and "try {" not in combined

with tempfile.TemporaryDirectory(prefix="lux-inspector-codegen-") as directory:
    root = Path(directory)
    config["output_root"] = str(root / "output")
    cfg, ir = root / "config.json", root / "input.json"
    cfg.write_text(json.dumps(config), encoding="utf-8")
    ir.write_text(json.dumps(data), encoding="utf-8")
    command = [sys.executable, str(script), "--config", str(cfg), "--ir", str(ir)]
    subprocess.run(command, check=True)
    before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in (root / "output").iterdir()}
    subprocess.run(command, check=True)
    assert before == {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in (root / "output").iterdir()}
    cases = [("widget = unapproved", "unknown widget"),
             ("widget = slider", "requires min/max"),
             ("widget = slider, min = 10, max = 1", "reversed range"),
             ("speed = nan", "non-finite"), ("widget = input, step = 0", "non-positive step")]
    for attributes, expected in cases:
        invalid = copy.deepcopy(data)
        field = next(d for d in invalid["declarations"] if d.get("fq_name") ==
                     "inspector_fixture::SmallComponent::number")
        field["attributes"] = [attributes]
        ir.write_text(json.dumps(invalid), encoding="utf-8")
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode != 0 and expected in result.stderr, result.stderr
        assert before == {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in (root / "output").iterdir()}
        print("exact rejection and output preservation:", expected)
    invalid = copy.deepcopy(data)
    field = next(d for d in invalid["declarations"] if d.get("fq_name") == "inspector_fixture::Component::nested")
    field["attributes"] = ["widget = slider, min = 0, max = 10"]
    ir.write_text(json.dumps(invalid), encoding="utf-8")
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode != 0 and "cannot edit aggregate" in result.stderr
    assert before == {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in (root / "output").iterdir()}
    print("exact rejection and output preservation: scalar widget on aggregate")
    invalid = copy.deepcopy(config)
    invalid["components"].append(invalid["components"][0])
    try:
        module.generate(invalid, data)
    except ValueError as error:
        assert "duplicate component" in str(error)
    else:
        raise AssertionError("duplicate component accepted")
print("Codegen direct calls, one cpp per component, deterministic no-op and seven exact negatives PASS")
