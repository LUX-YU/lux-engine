"""Conservative evidence-reuse gate for the F3 performance-only completion.

This is not a filename-based general test selector. Any changed build input, unknown
file category, production tree, or locked artifact forces integration review. It
checks actual Ninja inputs/deps/commands, CMake File API and CTest commands. A future
production change must provide its affected-target/test mapping instead of using
this script to waive tests.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'archive', 'lock', 'output', 'build', 'asan', 'ninja'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--base', default='41b4f25d973239098aac801f147daf09ab3ac640')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)

    def git(*command):
        return subprocess.check_output(['git', '-C', str(args.source), *command]).decode().strip()

    if git('status', '--porcelain', '--untracked-files=all'):
        raise RuntimeError('Qualification source must be clean')
    files = git('diff', '--name-only', args.base, 'HEAD').splitlines()
    manifest = json.loads((args.archive / 'manifest.json').read_text())
    for name, expected in manifest.items():
        if digest(args.archive / name) != expected:
            raise RuntimeError('Historical evidence changed: ' + name)
    original_binaries = json.loads((args.archive / 'performance/performance.json').read_text())['binaries']
    for name, expected in original_binaries.items():
        if digest(Path(name)) != expected:
            raise RuntimeError('Original benchmark binary changed: ' + name)
    lock = json.loads(args.lock.read_text())
    trees = ['modules', 'engine', 'render_legacy', 'docs/render-v2/final']
    if any(git('rev-parse', 'HEAD:' + p) != lock['trees'][p] for p in trees):
        raise RuntimeError('Production or frozen tree changed; affected integration required')
    report = {'source_commit': git('rev-parse', 'HEAD'), 'base': args.base,
              'production_implementation': '0fbbbd6db3aa4ad53283522e4a54d96556b5b478',
              'old_evidence_manifest_sha256': digest(args.archive / 'manifest.json'),
              'original_benchmark_hashes': original_binaries,
              'files': {}, 'dependencies': {}, 'reused': [], 'must_run': [], 'not_run': []}
    unknown = []
    for name in files:
        is_new_document = name.startswith('docs/render-v2/F3_') and not git('ls-tree', args.base, '--', name)
        is_diagnostic = name in ('cmake/render-v2-bootstrap/diagnose_f3.py',
                                 'cmake/render-v2-bootstrap/test_impact_f3.py')
        if not (is_new_document or is_diagnostic):
            unknown.append(name)
        report['files'][name] = {'old_git_object': git('ls-tree', args.base, '--', name),
                                'new_git_object': git('ls-tree', 'HEAD', '--', name),
                                'dependency_hits': []}
    for mode, build in [('build', args.build), ('asan', args.asan)]:
        for name, expected in lock['build_artifacts'][mode].items():
            if digest(build / name) != expected:
                raise RuntimeError('Locked artifact changed: ' + mode + '/' + name)
        texts = {}
        for operation in ('inputs', 'deps', 'commands'):
            command = [str(args.ninja), '-C', str(build), '-t', operation]
            if operation != 'deps':
                command.append('all')
            result = subprocess.run(command, capture_output=True)
            if result.returncode:
                raise RuntimeError(result.stderr.decode(errors='replace'))
            filename = mode + '-ninja-' + operation + '.txt'
            (args.output / filename).write_bytes(result.stdout)
            texts[filename] = result.stdout.decode(errors='replace')
        prefix = '' if mode == 'build' else 'asan-'
        for name in ('closure/closure.json', 'closure/tests.json', 'compile_commands.json'):
            texts[prefix + name] = (args.archive / (prefix + name)).read_text()
        for path in (build / '.cmake/api/v1/reply').glob('*.json'):
            texts[mode + '-file-api/' + path.name] = path.read_text()
        for name in files:
            # Normalize Windows/JSON path spelling. Search full relative paths, then
            # basenames conservatively to catch generated command relative references.
            for origin, text in texts.items():
                normalized = text.replace('\\', '/').lower()
                if name.lower() in normalized or Path(name).name.lower() in normalized:
                    report['files'][name]['dependency_hits'].append(origin)
        report['dependencies'][mode] = {'input_records': len(texts),
                                        'locked_artifacts': len(lock['build_artifacts'][mode]),
                                        'ninja_evidence': {n: digest(args.output / n) for n in texts if n.endswith('.txt')}}
        # Historical commands/results are referenced, never rerun or relabelled as new tests.
        for kind in ('test', 'closure'):
            name = prefix + kind + '.json'
            receipt = json.loads((args.archive / name).read_text())
            if receipt['exit_code'] != 0:
                raise RuntimeError('Prior qualification failed: ' + name)
            report['reused'].append({'evidence': str(args.archive / name), 'sha256': digest(args.archive / name),
                                      'reason': 'unchanged production trees, dependency inputs and locked artifacts'})
    hits = [name for name, value in report['files'].items() if value['dependency_hits']]
    report['integration_required'] = bool(unknown or hits)
    report['unknown_files'] = unknown
    report['must_run'] = ['diagnostic script syntax/integrity', 'independent 256 AA/BB/AB paired qualification',
                          'limited 128/512 controls', 'evidence/protection/hash checks']
    report['not_run'] = ['137 CTest, ASan and GPU reruns: historical PASS reused; no affected production path',
                         'PMU counters: xperf denied access (separate attempt receipt)',
                         'F4 Native Graph, Runtime and product: not authorized/implemented']
    report['status'] = 'INTEGRATION_REVIEW_REQUIRED' if report['integration_required'] else 'REUSE_ELIGIBLE'
    (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({'status': report['status'], 'files': len(files), 'dependency_hits': hits}, indent=2))
    if report['integration_required']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
