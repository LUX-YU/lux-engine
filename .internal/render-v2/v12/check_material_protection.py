"""v12 directory migration supplement; prior checkers remain byte-identical.
Only the two-line pure Protocol fixture migration in IPC is additionally permitted.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def git(root, *args, data=None):
    return subprocess.check_output(['git', '-C', str(root), *args], input=data, text=True).strip()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


# Derived from the unchanged prior checker; the only new external allowance is MaterialGraph CMake.
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = args.source.resolve()
records = root / '.internal/render-v2/v12'
snapshot = json.loads((records / 'snapshot.json').read_text())
base = snapshot['baseline']
manifest = json.loads((root / 'docs/render-v2/LEGACY_SOURCE_MANIFEST.json').read_text())
files = manifest['files']
assert len(files) == 719
actual = git(root, 'hash-object', '--stdin-paths', data='\n'.join(f['destination'] for f in files)+'\n').splitlines()
assert actual == [f['blob'] for f in files], 'Legacy working bytes changed'
tree = {r.split('\t')[1]: r.split('\t')[0].split()[2]
        for r in git(root, 'ls-tree', '-rz', 'HEAD').split('\0') if r}
assert all(tree[f['destination']] == f['blob'] for f in files), 'Legacy committed blob changed'
for protected in snapshot['protected']:
    repo = Path(protected['path'])
    assert git(repo, 'rev-parse', 'HEAD') == protected['head'], str(repo)
    assert git(repo, 'status', '--porcelain') == protected['status'], str(repo)
    for name, expected in protected['files'].items():
        assert sha(repo/name) == expected, str(repo/name)
for dependency in snapshot['dependencies'].values():
    assert git(dependency['path'], 'rev-parse', 'HEAD') == dependency['sha']
    assert git(dependency['path'], 'status', '--porcelain') == dependency['status']

changes = set(git(root, 'diff', '--name-only', base).splitlines())
changes.update(git(root, 'ls-files', '--others', '--exclude-standard').splitlines())
consumer_files = set(json.loads((records/'consumer-files.json').read_text()))
allowed = ('modules/function/render/', 'modules/core/execution/', 'engine/process/execution/',
           'cmake/render-v2-bootstrap/', '.internal/render-v2/v12/')
extras = {'modules/function/material/CMakeLists.txt', 'modules/core/CMakeLists.txt', 'engine/process/CMakeLists.txt', 'modules/platform/ipc/test/Ipc.cpp'}
assert all(p.startswith(allowed) or p in consumer_files or p in extras for p in changes), sorted(
    p for p in changes if not (p.startswith(allowed) or p in consumer_files or p in extras))
assert not git(root, 'diff', base, '--', 'render_legacy', 'editor_legacy', 'docs/render-v2')
ipc_fixture = 'modules/platform/ipc/test/Ipc.cpp'
ipc_changes = git(root, 'diff', '--name-only', base, '--', 'modules/platform/ipc').splitlines()
assert not (set(ipc_changes) - {ipc_fixture}), ipc_changes
original_fixture = git(root, 'show', f'{base}:{ipc_fixture}')
expected_fixture = original_fixture.replace(
    '        conflict.version.minor++;\n        check(!compatibleCommands(descriptor, conflict), "version rejected by pure protocol");',
    '        conflict.response.type = lux::cxx::typeToken<Ack>();\n        check(!compatibleCommands(descriptor, conflict), "response type rejected by pure protocol");'
)
assert expected_fixture != original_fixture
assert (root/ipc_fixture).read_text().strip() == expected_fixture, 'Unexpected IPC fixture edit'
# The original v12 checker is itself frozen at the last qualified checkpoint.
original_checker = git(root, 'show', '3df7ad7b04aaf2f694b9ce149e1fed5bdc1deeda:.internal/render-v2/v12/check_protection.py')
assert (records/'check_protection.py').read_text().strip() == original_checker
historical = [p for p in changes if p.startswith('.internal/') and not p.startswith('.internal/render-v2/v12/')]
assert not historical, historical
result = dict(status='PASS', baseline=base, head=git(root,'rev-parse','HEAD'), legacy_blobs=719,
              protected_files=sum(len(p['files']) for p in snapshot['protected']), changed_files=sorted(changes))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k!='changed_files'}))
