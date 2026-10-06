"""Qualify the active compile commands and the compiler's actual RTTI rejection."""
import json
from pathlib import Path
import re
import subprocess
import sys

build = Path(sys.argv[1])
compiler = sys.argv[2]
msvc = sys.argv[3] == 'MSVC'
commands = json.loads((build / 'compile_commands.json').read_text())
checked = 0
for item in commands:
    command = item['command']
    source = item['file'].replace('\\', '/')
    if '/third_party/' in source or '/_deps/' in source:
        continue
    if not source.endswith(('.cpp', '.cxx', '.cc')):
        continue
    required = ('/GR-', '/we4541') if msvc else ('-fno-rtti',)
    for flag in required:
        assert flag in command, (source, 'missing', flag)
    assert not re.search(r'(?:^|\s)(?:/GR(?:\s|$)|-frtti(?:\s|$))', command), source
    checked += 1
assert checked, 'No active C++ compilation was checked'

directory = build / 'no-rtti-probes'
directory.mkdir(exist_ok=True)
base = '#include <typeinfo>\nstruct B { virtual ~B() = default; }; struct D : B {};\n'
cases = {
    'positive': base + 'int probe(B* b) { return b != nullptr; }\n',
    'downcast': base + 'D* probe(B* b) { return dynamic_cast<D*>(b); }\n',
    'typeid': base + 'const std::type_info& probe(B& b) { return typeid(b); }\n',
}
for name, text in cases.items():
    source = directory / (name + '.cpp')
    source.write_text(text)
    if msvc:
        args = [compiler, '/nologo', '/c', '/std:c++20', '/GR-', '/we4541',
                '/Fo' + str(directory / (name + '.obj')), str(source)]
    else:
        args = [compiler, '-c', '-std=c++20', '-fno-rtti', str(source),
                '-o', str(directory / (name + '.o'))]
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    print(name, result.returncode, result.stdout)
    if name == 'positive':
        assert result.returncode == 0
    else:
        assert result.returncode != 0
        assert ('4541' in result.stdout if msvc else 'rtti' in result.stdout.lower()), result.stdout
print('PASS:', checked, 'active C++ units disable RTTI; positive and two compiler negatives qualified')
