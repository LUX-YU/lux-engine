"""Read-only architecture convergence R2 protection audit. Historical checkers remain unchanged."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--repo', type=Path, default=Path.cwd())
p.add_argument('--build', type=Path)
p.add_argument('--clean', action='store_true')
p.add_argument('--user-worktree', type=Path)
args = p.parse_args()
repo = args.repo.resolve()
baseline = '6becd3a8aca68d73395b8b9329638fd3fc9b25e2'


def git(*argv, data=None):
    return subprocess.check_output(['git', '-C', str(repo), *argv], input=data).decode().strip()


def tree(rev):
    return {e.split('\t', 1)[1]: e.split('\t')[0].split()[2]
            for e in git('ls-tree', '-rz', rev).split('\0') if e}


def allowed(path):
    return path in {
        'modules/function/render/runtime/CMakeLists.txt',
        'modules/function/render/runtime/README.md',
        'modules/function/render/runtime/include/lux/engine/render/runtime/RenderClient.hpp',
        'modules/function/render/runtime/pinclude/Wire.hpp',
        'modules/function/render/runtime/src/RenderClient.cpp',
        'modules/function/render/runtime/src/RenderServer.cpp',
        'modules/function/render/runtime/src/Wire.cpp',
        'modules/function/render/runtime/test/Lifetimes.cpp',
        'modules/function/render/runtime/test/Offscreen.cpp',
        'modules/function/render/runtime/test/Programs.cpp',
        'modules/function/render/runtime/test/Runtime.cpp',
        'modules/function/render/runtime/test/Shutdown.cpp',
        'modules/function/render/runtime/test/installed/Main.cpp',
        'modules/platform/ipc/include/lux/engine/platform/ipc/IpcMessage.hpp',
        'modules/platform/ipc/src/Ipc.cpp',
        'modules/platform/ipc/test/Attachments.cpp',
        'modules/platform/ipc/test/Ipc.cpp',
        'modules/platform/ipc/test/PluginA.cpp',
        'modules/platform/ipc/test/installed/Main.cpp',
        'modules/function/render/runtime/test/CompletionTest.hpp',
        '.internal/editor-redesign/check_render_convergence_r2.py',
        '.internal/editor-redesign/render-convergence-r2.md',
    }


old, current = tree(baseline), tree('HEAD')
changed = git('diff', '--name-only', baseline, '--').splitlines()
untracked = git('ls-files', '--others', '--exclude-standard').splitlines()
assert all(allowed(f) for f in changed + untracked), changed + untracked
status = git('status', '--porcelain')
if args.clean:
    assert not status, status
ipc_path = 'modules/platform/ipc/src/Ipc.cpp'
old_ipc = git('show', baseline + ':' + ipc_path)
new_ipc = (repo / ipc_path).read_text().strip()
start, end = '    IpcMessage::~IpcMessage()', '    IpcCore& IpcCore::instance()'
assert old_ipc.split(start)[0] == new_ipc.split(start)[0], 'IPC directory prefix changed'
assert old_ipc.split(end)[1] == new_ipc.split(end)[1], 'IPC Channel/Link/Ring algorithm changed'
protected = [f for f in old if not allowed(f)]
filtered = git('hash-object', '--stdin-paths', data=('\n'.join(protected) + '\n').encode()).splitlines()
literal_matches = []
for path, blob in zip(protected, filtered, strict=True):
    assert old[path] == current[path], path
    if blob != old[path]:
        raw = (repo / path).read_bytes()
        assert hashlib.sha1(b'blob ' + str(len(raw)).encode() + b'\0' + raw).hexdigest() == old[path], path
        literal_matches.append(path)
manifest = json.loads((repo / 'docs/render-v2/LEGACY_SOURCE_MANIFEST.json').read_text())
assert manifest['file_count'] == len(manifest['files']) == 719
frozen = tree(manifest['source_sha'])
for item in manifest['files']:
    assert frozen[item['source']] == current[item['destination']] == item['blob'], item
user = {}
if args.user_worktree:
    for path, expected in {
        '.vscode/settings.json': 'f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c',
        'modules/function/render/graph/include/lux/engine/render/graph/Graph.hpp':
            '00c28abc7bd0a786666f101a5212400a5b467af6bdbbc5049c69d59d548c726a',
    }.items():
        user[path] = hashlib.sha256((args.user_worktree / path).read_bytes()).hexdigest()
        assert user[path] == expected, path
units = []
if args.build:
    for entry in json.loads((args.build / 'compile_commands.json').read_text()):
        source = entry['file'].replace('\\', '/')
        if '/modules/function/render/' not in source:
            continue
        command = entry['command'].replace('\\', '/').lower()
        for forbidden in ['render_legacy', '/engine/ecs', '/editor/', '/engine/toolchain']:
            assert forbidden not in command, (source, forbidden)
        assert 'c++20' in command and '/gr-' in command, source
        if '/native/' not in source and '/runtime/' not in source:
            assert 'vulkansdk' not in command and '/platform/ipc/' not in command, source
        units.append(source)
    assert units
print(json.dumps({'head': git('rev-parse', 'HEAD'), 'baseline': baseline,
                  'legacy_blobs': 719, 'protected_files': len(protected), 'literal_matches': literal_matches,
                  'user_sha256': user, 'changes': changed + untracked, 'status': status,
                  'compile_units': units, 'protection': 'PASS'}, indent=2))
