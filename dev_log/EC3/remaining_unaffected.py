"""Select only remaining tests whose actual PE imports avoid the quarantined artifact.

This is an additional partial run, not a substitute for the complete required suite.
It never rebuilds or restores an artifact and never changes security configuration.
"""
from pathlib import Path
import hashlib, json, re, subprocess, sys

w = Path(__file__).resolve().parent
c = json.loads((w / 'final-config.json').read_text())
b = Path(c['build'])
roots = [b / 'bin', Path(c['prefix']) / 'bin', Path('D:/Development/vcpkg/installed/x64-windows/bin')]
blocked = {'lux_engine_scene_composition.dll'}
cache = {}
def closure(path, active=None):
    active = set() if active is None else active
    key = str(path).lower()
    if key in active:
        return set()
    active.add(key)
    if key not in cache:
        out = subprocess.check_output(['dumpbin', '/dependents', str(path)], text=True, errors='replace')
        names = re.findall(r'^\s+(\S+\.dll)\s*$', out, re.M | re.I)
        cache[key] = {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'imports': names}
    result = set()
    for name in cache[key]['imports']:
        if name.lower() in blocked:
            result.add(name.lower())
            continue
        target = next((root / name for root in roots if (root / name).exists()), None)
        if target:
            result.update(closure(target, active))
    return result

tests = json.loads((w / 'logs/final-test-list.log').read_text())['tests']
log = (w / 'logs/final-ctest.log').read_text(errors='replace')
passed = set(re.findall(r'Test\s+#\d+:\s+(\S+)\s+\.+\s+Passed', log))
selected, records = [], []
for test in tests:
    if test['name'] in passed or 'native_input' in test['name']:
        continue
    exe = Path(test['command'][0])
    if exe.parent != b / 'bin':
        continue
    missing = sorted(closure(exe))
    records.append({'test': test['name'], 'blocked_imports': missing})
    if not missing:
        selected.append(test['name'])
(w / 'remaining-test-imports.json').write_text(json.dumps({'tests': records, 'actual_pe_imports': list(cache.values())}, indent=2)+'\n')
print('Unblocked remaining tests:', len(selected), 'blocked:', len(records)-len(selected), flush=True)
assert selected
if '--inspect-only' in sys.argv:
    sys.exit(0)
result = subprocess.run(['ctest', '--test-dir', str(b), '--output-on-failure', '-j', '1', '-R',
                         '^(' + '|'.join(re.escape(name) for name in selected) + ')$'])
sys.exit(result.returncode)
