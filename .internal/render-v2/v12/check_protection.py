"""v12 protection check; prior checkers and qualifications are read-only."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def git(root, *args, data=None):
    return subprocess.check_output(['git', '-C', str(root), *args], input=data, text=True).strip()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
extras = {'modules/core/CMakeLists.txt', 'engine/process/CMakeLists.txt'}
assert all(p.startswith(allowed) or p in consumer_files or p in extras for p in changes), sorted(
    p for p in changes if not (p.startswith(allowed) or p in consumer_files or p in extras))
assert not git(root, 'diff', base, '--', 'render_legacy', 'editor_legacy', 'docs/render-v2', 'modules/platform/ipc')
historical = [p for p in changes if p.startswith('.internal/') and not p.startswith('.internal/render-v2/v12/')]
assert not historical, historical
result = dict(status='PASS', baseline=base, head=git(root,'rev-parse','HEAD'), legacy_blobs=719,
              protected_files=sum(len(p['files']) for p in snapshot['protected']), changed_files=sorted(changes))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k!='changed_files'}))
