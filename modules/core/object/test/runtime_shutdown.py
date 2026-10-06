"""Check the actual DLL shutdown, after the executable's atexit callbacks."""

import subprocess
import sys

result = subprocess.run([sys.argv[1], "--shutdown"], capture_output=True, text=True)
print(result.stdout, end="")
print(result.stderr, end="", file=sys.stderr)
if result.returncode:
    raise SystemExit(result.returncode)
expected = "Runtime shutdown: discarded without dispatch; CLOSED retained"
if result.stdout.count(expected) != 1:
    raise SystemExit("Runtime did not destroy exactly one accepted payload during shutdown")
