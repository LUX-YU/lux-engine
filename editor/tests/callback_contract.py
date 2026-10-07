"""Compile the real SDK callback sites: a positive control and seven throwing negatives."""
import ctypes
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

build = Path(sys.argv[1])
commands = json.loads((build / 'compile_commands.json').read_text())
item = next(c for c in commands if c['file'].replace('\\', '/').endswith('/callback_contract.cpp'))
if os.name == 'nt':
    parse = ctypes.windll.shell32.CommandLineToArgvW
    parse.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    parse.restype = ctypes.POINTER(ctypes.c_wchar_p)
    count = ctypes.c_int()
    data = parse(item['command'], ctypes.byref(count))
    args = [data[i] for i in range(count.value)]
    ctypes.windll.kernel32.LocalFree(data)
else:
    args = shlex.split(item['command'])
directory = build / 'callback-contract-probes'
directory.mkdir(exist_ok=True)
msvc = any(a.startswith('/Fo') for a in args)
for case in range(8):
    output = directory / (str(case) + ('.obj' if msvc else '.o'))
    invocation = list(args)
    if msvc:
        invocation = ['/Fo' + str(output) if a.startswith('/Fo') else a for a in invocation]
        invocation.append('/DLUX_CALLBACK_CASE=' + str(case))
    else:
        invocation[invocation.index('-o') + 1] = str(output)
        invocation.append('-DLUX_CALLBACK_CASE=' + str(case))
    result = subprocess.run(invocation, cwd=item['directory'], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace')
    print(case, result.returncode, result.stdout)
    assert (result.returncode == 0) == (case == 0), 'Incorrect acceptance for callback case ' + str(case)
    if case:
        assert ('noexcept' in result.stdout or 'constraints' in result.stdout), result.stdout
print('PASS: Assembly, UI, service, scene tool, capture, enumeration and scene profile reject throwing callbacks')
