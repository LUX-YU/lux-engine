"""Qualify real transfer destruction against native wait outcomes."""
import os
import subprocess
import sys

if os.name == "nt":
    import ctypes
    ctypes.windll.kernel32.SetErrorMode(0x8003)

for mode in ("success", "lost", "failed"):
    result = subprocess.run([sys.argv[1], mode], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    if mode == "failed":
        assert result.returncode != 0, output
        assert "reached transfer idle boundary" in output, output
        assert "Transfer backing wait-idle failed before native release" in output, output
    else:
        assert result.returncode == 0, output
        assert "PASS transfer native idle retirement boundary" in output, output
    print(mode, result.returncode, output, flush=True)
