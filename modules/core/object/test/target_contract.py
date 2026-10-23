import subprocess
import sys
import signal

abort_codes = {3, 0x40000015, 0xC0000409} if sys.platform == "win32" else {-signal.SIGABRT}

for mode, marker, failure in [
    ('--destroy', 'Destroying borrowed target', True),
    ('--shutdown', 'Target shutdown: completed once with nullptr; CLOSED rejected', False),
]:
    result = subprocess.run([sys.argv[1], mode], capture_output=True, text=True, timeout=30)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    assert result.stdout.count(marker) == 1
    if failure:
        assert result.returncode in abort_codes, (mode, result.returncode)
    else:
        assert result.returncode == 0, (mode, result.returncode)
print('PASS: active callback destruction rejected; accepted completion cancelled exactly once at shutdown')
