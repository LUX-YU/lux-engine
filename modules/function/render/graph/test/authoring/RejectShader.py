"""Compile actual negative shaders and require precise production reconciliation failures."""
import json
from pathlib import Path
import re
import subprocess
import sys

compiler, consumer, original, output_dir, case = sys.argv[1:]
out = Path(output_dir) / case
out.mkdir(parents=True, exist_ok=True)
source = Path(original).read_text()
expected = ""
if case == "offset":
    source = re.sub(r"offset=(\d+)", lambda m: "offset=" + str(int(m[1]) + 4), source)
    expected = "Scalar offset mismatch"
elif case == "extra_scalar":
    source = source.replace("} lux_push;", "    float undeclared;\n} lux_push;")
    expected = "Shader scalar field coverage mismatch"
elif case == "descriptor_count":
    source = source.replace("p_inputs[2]", "p_inputs[3]")
    expected = "Descriptor type/count mismatch"
elif case == "access":
    source = source.replace("readonly buffer", "buffer")
    expected = "Resource access mismatch"
elif case == "runtime_array":
    source = source.replace("data[]", "data[1]")
    expected = "Storage runtime array mismatch"
elif case == "extra_resource":
    source = source.replace("//! lux-shader", "layout(set=0, binding=42) uniform texture2D undeclared;\n//! lux-shader")
    expected = "Shader has resources outside PassSchema"
else:
    raise ValueError(case)
path = out / "candidate.glsl"
path.write_text(source)
spirv = out / "candidate.spv"
stage = "compute" if "stage=compute" in source else "fragment"
commands = []
for args in ([compiler, "-fshader-stage=" + stage, "--target-env=vulkan1.2", str(path), "-o", str(spirv)],
             [consumer, str(spirv), "validate"]):
    result = subprocess.run(args, capture_output=True)
    commands.append({"command": args, "exit_code": result.returncode,
                     "output": (result.stdout + result.stderr).decode(errors="replace")})
    if len(commands) == 1 and result.returncode:
        raise RuntimeError(commands[-1])
(out / "commands.json").write_text(json.dumps(commands, indent=2))
if commands[-1]["exit_code"] == 0 or expected not in commands[-1]["output"]:
    raise RuntimeError(commands[-1])
print(case + ": expected rejection: " + commands[-1]["output"])
