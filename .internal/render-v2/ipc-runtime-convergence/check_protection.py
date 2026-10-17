"""New contract boundary; historical v12 checkers retain their original meaning."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

BASE = '9184c9f71c63af7f686822c76b89263b29ef7a63'
RECORDS = '.internal/render-v2/ipc-runtime-convergence/'
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = args.source.resolve()

def git(repo, *arguments, data=None):
    return subprocess.check_output(['git', '-C', str(repo), *arguments], input=data, text=True).strip()

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

manifest = json.loads((root/'docs/render-v2/LEGACY_SOURCE_MANIFEST.json').read_text())['files']
assert len(manifest) == 719
blobs = git(root, 'hash-object', '--stdin-paths', data='\n'.join(row['destination'] for row in manifest)+'\n').splitlines()
assert blobs == [row['blob'] for row in manifest], 'Legacy working bytes changed'
tree = {row.split('\t')[1]:row.split('\t')[0].split()[2]
        for row in git(root, 'ls-tree', '-rz', 'HEAD').split('\0') if row}
assert all(tree[row['destination']] == row['blob'] for row in manifest), 'Legacy committed blobs changed'
snapshot = json.loads((root/'.internal/render-v2/v12/snapshot.json').read_text())
for protected in snapshot['protected']:
    repo = Path(protected['path'])
    assert git(repo, 'rev-parse', 'HEAD') == protected['head'], str(repo)
    assert git(repo, 'status', '--porcelain') == protected['status'], str(repo)
    for path, expected in protected['files'].items():
        assert digest(repo/path) == expected, str(repo/path)
extra = Path('E:/RenderV2Rebuild/V12/source/modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp')
assert digest(extra) == '80a98f9e08ff0eb18b5f6ae0b3dea862f59384b71d9d36efe9c041acbec957df'
for dependency in snapshot['dependencies'].values():
    assert git(dependency['path'], 'rev-parse', 'HEAD') == dependency['sha']
    assert git(dependency['path'], 'status', '--porcelain') == dependency['status']
changes = set(git(root, 'diff', '--name-only', BASE).splitlines())
changes.update(git(root, 'ls-files', '--others', '--exclude-standard').splitlines())
allowed = ('modules/platform/ipc/', 'modules/function/render/', 'modules/core/execution/',
           'cmake/render-v2-bootstrap/', RECORDS)
assert all(path.startswith(allowed) for path in changes), sorted(path for path in changes if not path.startswith(allowed))
frozen = [path for path in changes if (path.startswith('.internal/') and not path.startswith(RECORDS))
          or Path(path).name.startswith(('VERIFICATION', 'REPORT'))]
assert not frozen, frozen
assert not git(root, 'diff', BASE, '--', 'render_legacy', 'editor_legacy', 'docs/render-v2', 'AGENTS.md')
args.output.parent.mkdir(parents=True, exist_ok=True)
result = dict(baseline=BASE, head=git(root,'rev-parse','HEAD'), legacy_blobs=719,
              protected_user_files=4, changed_files=sorted(changes), result='PASS')
args.output.write_text(json.dumps(result, indent=2)+'\n')
print('PASS: 719 Legacy blobs; four protected user files; dependencies; historical files; convergence boundary')
