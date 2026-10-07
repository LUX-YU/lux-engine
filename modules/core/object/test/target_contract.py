import subprocess
import sys

for mode, marker, failure in [
    ('--destroy', 'Destroying borrowed target', True),
    ('--shutdown', 'Target shutdown: completed once with nullptr; CLOSED rejected', False),
]:
    result = subprocess.run([sys.argv[1], mode], capture_output=True, text=True, timeout=30)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    assert result.stdout.count(marker) == 1
    assert bool(result.returncode) == failure, (mode, result.returncode)
print('PASS: active callback destruction rejected; accepted completion cancelled exactly once at shutdown')
