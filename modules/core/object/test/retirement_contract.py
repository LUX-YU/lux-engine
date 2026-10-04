"""Real process death checks for the object queue's final affinity boundary."""
import subprocess
import sys

executable = sys.argv[1]
positive = subprocess.run([executable], capture_output=True, text=True, timeout=30)
assert positive.returncode == 0, positive.stdout + positive.stderr
for mode in ("--reject-held", "--reject-dispatch", "--reject-foreign"):
    result = subprocess.run([executable, mode], capture_output=True, text=True, timeout=30)
    assert "reached final safe-point contract" in result.stdout, result.stdout + result.stderr
    assert result.returncode != 0, f"{mode}: invalid lifetime was accepted"
    print(mode, "rejected", result.returncode)
