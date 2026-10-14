import argparse, hashlib, json, subprocess
from pathlib import Path

BASE = 'd29b6d448b9f7527431ead5fb8765457ce28875d'

def run(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args]).decode().strip()

def tree(root, ref):
    result = {}
    for line in run(root, 'ls-tree', '-r', ref).splitlines():
        info, name = line.split('\t', 1)
        result[name] = info
    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--clean', action='store_true')
    args = parser.parse_args()
    root = args.root.resolve()
    before, after = tree(root, BASE), tree(root, 'HEAD')
    for name in before.keys() | after.keys():
        if not name.startswith('modules/platform/ipc/'):
            assert before.get(name) == after.get(name), name
    status = run(root, 'status', '--porcelain', '--untracked-files=all')
    if args.clean:
        assert not status, status
    for line in status.splitlines():
        # strip() removes the first status space, so use suffix matching for the sole user file.
        assert line.endswith('.vscode/settings.json') or 'modules/platform/ipc/' in line, line
    manifest = json.loads((root/'docs/render-v2/LEGACY_SOURCE_MANIFEST.json').read_text())
    assert len(manifest['files']) == 719
    frozen = tree(root, manifest['source_sha'])
    paths = '\n'.join(item['destination'] for item in manifest['files']) + '\n'
    blobs = subprocess.check_output(['git', '-C', str(root), 'hash-object', '--stdin-paths'], input=paths.encode()).decode().splitlines()
    for item, blob in zip(manifest['files'], blobs):
        name = item['destination']
        assert after[name].split()[2] == item['blob'] == frozen[item['source']].split()[2]
        assert blob == item['blob'], name
    source = (root/'modules/platform/ipc/src/Ipc.cpp').read_text()
    headers = '\n'.join(p.read_text() for p in (root/'modules/platform/ipc/include').rglob('*.hpp'))
    cmake = (root/'modules/platform/ipc/CMakeLists.txt').read_text()
    for forbidden in ('std::mutex', 'condition_variable', 'atomic_flag', '.wait(', '.notify_', 'tryPopValue(',
                      'link->refs', 'link_->refs', 'link->access', 'channel_->refs', 'unordered_map',
                      'lux/engine/render', 'vulkan/', 'stdexec', 'std::thread'):
        assert forbidden not in source + headers, forbidden
    assert 'LUX_IPC_INLINE_CAPACITY' not in headers + cmake
    assert 'Result<std::optional' not in headers
    # ReceivedRequest methods and successful receive/reply paths cannot touch role retention.
    hot = source[source.index('    EIpcReceive IpcResponder::tryReceive'):source.index('#ifdef LUX_IPC_TESTING\n    namespace testing')]
    assert 'role_uses' not in hot and 'releaseChannel' not in hot and 'fetch_add' not in hot
    print(json.dumps({'head': run(root, 'rev-parse', 'HEAD'), 'baseline': BASE, 'legacy_blobs': 719,
                      'historical_and_non_ipc_files': 'unchanged', 'status': status,
                      'source_sha256': hashlib.sha256(source.encode()).hexdigest()}, indent=2))

if __name__ == '__main__':
    main()
