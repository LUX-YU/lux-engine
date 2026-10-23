"""Real process death checks for the object queue's final affinity boundary."""
import subprocess
import sys
import signal

# CRT abort/fast-fail on Windows; SIGABRT on POSIX. An access violation is not a contract rejection.
abort_codes = {3, 0x40000015, 0xC0000409} if sys.platform == "win32" else {-signal.SIGABRT}

executable = sys.argv[1]
positive = subprocess.run([executable], capture_output=True, text=True, timeout=30)
assert positive.returncode == 0, positive.stdout + positive.stderr
for mode in ("--reject-held", "--reject-dispatch", "--reject-foreign"):
    result = subprocess.run([executable, mode], capture_output=True, text=True, timeout=30)
    assert "reached final safe-point contract" in result.stdout, result.stdout + result.stderr
    assert result.returncode in abort_codes, f"{mode}: expected abort, got {result.returncode}"
    print(mode, "rejected", result.returncode)
