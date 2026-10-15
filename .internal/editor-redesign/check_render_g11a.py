"""G11A exact-baseline protection. Does not rewrite historical qualification."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--repo', type=Path, default=Path.cwd())
parser.add_argument('--snapshot', type=Path, required=True)
parser.add_argument('--user-worktree', type=Path, required=True)
parser.add_argument('--clean', action='store_true')
args = parser.parse_args()
repo = args.repo.resolve()
baseline = '9a2154371018b082dc2c0270a2c898559d69e213'
snapshot = json.loads(args.snapshot.read_text())

def git(*arguments, data=None):
    return subprocess.check_output(['git', '-C', str(repo), *arguments], input=data).decode().strip()

def tree(rev):
    return {e.split('\t', 1)[1]: e.split('\t')[0].split()[2]
            for e in git('ls-tree', '-rz', rev).split('\0') if e}

allowed = {
    '.internal/editor-redesign/G11A_ARCHITECTURE_FREEZE.md',
    '.internal/editor-redesign/SCENE_WORKING_COLOR_SPACE.md',
    '.internal/editor-redesign/check_render_g11a.py',
    '.internal/editor-redesign/G11A_IMPLEMENTATION.md',
    '.internal/editor-redesign/G11A_VERIFICATION.md',
    'modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/DeviceSession.hpp',
    'modules/function/render/native/vulkan/src/DeviceSession.cpp',
    'modules/function/render/native/vulkan/src/memory/Memory.cpp',
    'modules/function/render/native/vulkan/test/Transfer.cpp',
    'modules/function/render/native/vulkan/test/CMakeLists.txt',
    'modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp',
    'modules/function/render/runtime/pinclude/Deferred.hpp',
    'modules/function/render/runtime/src/RenderServer.cpp',
    'modules/function/render/runtime/src/Request.cpp',
    'modules/function/render/runtime/test/CMakeLists.txt',
    'modules/function/render/runtime/test/GpuReady.cpp',
}
old = tree(baseline)
assert old == snapshot['baseline_tree']
changes = set(git('diff', '--name-only', baseline).splitlines())
changes.update(git('ls-files', '--others', '--exclude-standard').splitlines())
assert changes <= allowed, sorted(changes - allowed)
protected = sorted(set(old) - allowed)
filtered = git('hash-object', '--stdin-paths', data=('\n'.join(protected) + '\n').encode()).splitlines()
for path, blob in zip(protected, filtered, strict=True):
    if blob != old[path]:
        raw = (repo / path).read_bytes()
        assert hashlib.sha1(b'blob ' + str(len(raw)).encode() + b'\0' + raw).hexdigest() == old[path], path
for path, expected in snapshot['user_sha256'].items():
    assert hashlib.sha256((args.user_worktree / path).read_bytes()).hexdigest() == expected, path
manifest = json.loads((repo / 'docs/render-v2/LEGACY_SOURCE_MANIFEST.json').read_text())
assert manifest['files'] == snapshot['legacy_files'] and manifest['file_count'] == 719
for item in manifest['files']:
    assert item['destination'] in protected and old[item['destination']] == item['blob']
status = git('status', '--porcelain')
if args.clean:
    assert not status, status
print(json.dumps({'baseline': baseline, 'head': git('rev-parse', 'HEAD'), 'legacy_blobs': 719,
                  'protected_files': len(protected), 'changes': sorted(changes),
                  'user_sha256': snapshot['user_sha256'], 'status': status, 'protection': 'PASS'}, indent=2))
