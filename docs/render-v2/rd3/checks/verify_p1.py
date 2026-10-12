"""Read-only P1 provenance/boundary checks. Run at exact I in a separate clone.

Behavioral correctness is established by C++/GPU tests, not these text checks.
Historical D0 gates remain bound to their own implementation commit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

BASE = '38262ecd3e962690703173397f49796cfd81292e'
LEGACY = 'a669409a289a6fa4092f21176397795b1cdb7f3e'
ZIP_SHA = '3ac411f5c6d000b1a7b7a3ffd0e8e3e0b0aeb45936a91433065f9cbc7ead88af'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--implementation', required=True)
    ap.add_argument('--zip', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    root = args.source.resolve()
    out = args.output.resolve()
    if out.is_relative_to(root):
        raise SystemExit('Evidence must be outside source')
    out.mkdir(parents=True, exist_ok=True)
    checks = []

    def check(name, passed):
        checks.append({'name': name, 'passed': bool(passed)})

    def git(*values):
        return subprocess.check_output(['git', '-C', str(root), *values])

    def tree(ref):
        result = {}
        for row in git('ls-tree', '-rlz', ref).split(b'\0'):
            if row:
                meta, path = row.split(b'\t')
                mode, kind, blob, size = meta.split()
                result[path.decode()] = (mode.decode(), blob.decode(), int(size))
        return result

    check('exact implementation', git('rev-parse', 'HEAD').decode().strip() == args.implementation)
    check('clean tracked tree', not git('status', '--porcelain', '--untracked-files=all'))
    before, current, frozen = tree(BASE), tree('HEAD'), tree(LEGACY)
    changes = sorted(p for p in before.keys() | current.keys() if before.get(p) != current.get(p))
    manifest = (root / 'docs/render-v2/rd3/P1_CHANGESET.txt').read_text().splitlines()
    check('exact declared change list', changes == manifest)
    native = 'modules/function/render/vulkan/'
    allowed_native = {native + 'include/lux/engine/render/vulkan/graph/Executable.hpp'}
    allowed_native |= {native + 'src/graph/' + p + '.cpp' for p in ('Compile', 'Record', 'Export', 'Diagnostics')}
    for path in changes:
        allowed = (path.startswith('modules/function/render/runtime/') or path in allowed_native
                   or path.startswith(native + 'test/')
                   or path in ('cmake/render-v2-bootstrap/CMakeLists.txt', 'cmake/render-v2-bootstrap/RD3Runtime.cmake')
                   or path.startswith('docs/render-v2/rd3/P1_')
                   or path == 'docs/render-v2/rd3/checks/verify_p1.py')
        check('allowed ' + path, allowed)
    check('only replaced owner implementation deleted', set(before) - set(current) == {
        'modules/function/render/runtime/src/Frame.cpp',
        'modules/function/render/runtime/pinclude/RuntimeBackend.hpp'})
    for path, entry in before.items():
        if path.startswith('docs/') or path.startswith('render_legacy/'):
            check('historical blob preserved ' + path, current.get(path) == entry)
    legacy = json.loads((root / 'docs/render-v2/LEGACY_SOURCE_MANIFEST.json').read_text())
    check('Legacy count 719', len(legacy['files']) == legacy['file_count'] == 719)
    for f in legacy['files']:
        expected = (f['mode'], f['blob'], f['bytes'])
        check('Legacy frozen ' + f['source'], frozen.get(f['source']) == expected)
        check('Legacy protected ' + f['destination'], current.get(f['destination']) == expected)
    # LF checkout, checked against Git blobs without changing worktree content.
    for path, (mode, blob, size) in current.items():
        data = (root / path).read_bytes()
        actual = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        check('checkout exact blob ' + path, actual == blob)
    d = root / 'docs/render-v2/rd3'
    for name in ('MANIFEST.sha256', 'D0_MANIFEST.sha256'):
        for line in (d / name).read_text(encoding='utf-8-sig').splitlines():
            if not line.strip():
                continue
            digest, path = line.split(maxsplit=1)
            path = path.lstrip('* ')
            check(name + ' ' + path, hashlib.sha256((d / path).read_bytes()).hexdigest() == digest)
    check('input ZIP SHA', hashlib.sha256(args.zip.read_bytes()).hexdigest() == ZIP_SHA)
    with zipfile.ZipFile(args.zip) as z:
        check('input ZIP CRC', z.testzip() is None)
    # These four Native TUs may only change the role names and extract submit.
    for filename in ('Compile', 'Record', 'Export', 'Diagnostics'):
        path = native + 'src/graph/' + filename + '.cpp'
        expected = git('show', BASE + ':' + path).decode()
        expected = expected.replace('ExecutableGraphPlan', 'Graph').replace('NativeGraphInvocations', 'GraphExecutionContext')
        if filename == 'Record':
            expected = expected.replace('GraphExecutionContext::submit(\n', 'executeGraph(\n        GraphExecutionContext& context,\n')
            start = expected.index('RenderResult<GraphSubmission> executeGraph(')
            end = expected.index('RenderResult<bool> GraphExecutionContext::completed', start)
            body = expected[start:end].replace('*backing_', '*context.backing_').replace('traceCapacity()', 'context.traceCapacity()')
            expected = expected[:start] + body + expected[end:]
        check('Native algorithm unchanged ' + filename, (root / path).read_text() == expected)
    runtime = root / 'modules/function/render/runtime'
    migrated = list((runtime / 'src').glob('*.cpp')) + list((runtime / 'include').rglob('*.hpp'))
    migrated += [runtime / 'pinclude/RuntimeChannel.hpp']
    for path in migrated:
        check('no hash object index ' + path.name, not re.search(r'std::unordered_(map|set)', path.read_text()))
    check('Client no Native dependency', 'vulkan' not in (runtime / 'src/Runtime.cpp').read_text().lower())
    gitdir = Path(git('rev-parse', '--absolute-git-dir').decode().strip()).resolve()
    common = Path(git('rev-parse', '--git-common-dir').decode().strip())
    if not common.is_absolute():
        common = root / common
    common = common.resolve()
    objects = [p for p in (common / 'objects').rglob('*') if p.is_file()]
    check('independent git directory', gitdir == common and common.is_relative_to(root))
    check('no object alternates', not (common / 'objects/info/alternates').exists())
    check('no hardlinked objects', bool(objects) and max(p.stat().st_nlink for p in objects) == 1)
    failures = [c['name'] for c in checks if not c['passed']]
    result = {'implementation': args.implementation, 'base': BASE,
              'status': 'FAIL' if failures else 'PASS', 'checks': len(checks),
              'failures': failures, 'changes': changes, 'legacy_count': len(legacy['files']),
              'git_dir': str(gitdir), 'common_dir': str(common)}
    (out / 'checks.json').write_text(json.dumps(checks, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
