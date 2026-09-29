"""P06 R1 gate: immutable evidence and Git blobs only; no producer-machine paths are opened."""
from pathlib import Path, PurePosixPath
import argparse, collections, hashlib, json, re, subprocess, zipfile


def assertions(source):
    result = []
    for match in re.finditer(r'\bassert\s*\(', source):
        start = match.end(); depth = 1; quote = None; escape = False
        for end in range(start, len(source)):
            char = source[end]
            if quote:
                if escape: escape = False
                elif char == '\\': escape = True
                elif char == quote: quote = None
            elif char in ('"', "'"): quote = char
            elif char == '(': depth += 1
            elif char == ')':
                depth -= 1
                if depth == 0:
                    result.append(re.sub(r'\s+', '', source[start:end])); break
        else: raise AssertionError('unterminated assertion')
    return collections.Counter(result)


def check(root):
    here = root / 'dev_log/P06-R1'
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def git(*args): return subprocess.check_output(['git', *args], cwd=root)
    def digest(data): return hashlib.sha256(data).hexdigest()
    def archived(name):
        path = PurePosixPath(name)
        assert not path.is_absolute() and '..' not in path.parts and ':' not in name and '\\' not in name
        result = root.joinpath(*path.parts).resolve(); assert result.is_relative_to(root / 'dev_log')
        return result
    r = read(here / 'receipt.json'); impl = r['implementation_sha']; base = r['input_sha']
    assert base == 'cc3f455095f52e9a2740bf0a31a55e6672240916'
    assert r['phase'] == 'P06-R1' and r['migration_stage'] == 'P06' and r['status'] == 'PASS'
    assert r['stop_after'] == 'P06' and not r['continuation_authorized'] and not r['new_bridges']
    git('merge-base', '--is-ancestor', base, impl); git('merge-base', '--is-ancestor', impl, 'HEAD')
    for item in read(here / 'artifacts.json'):
        assert digest(archived(item['archive_path']).read_bytes()) == item['sha256'], item
    files = read(here / 'files.json')
    assert {f['path'] for f in files} == set(git('diff', '--name-only', base, impl).decode().splitlines())
    for f in files: assert digest(git('show', impl + ':' + f['path'])) == f['git_content_sha256']
    protected = ['dev_log', 'editor/project', 'editor/editing', 'editor/persistence', 'editor/adapters',
                 'editor/tools/scene/model', 'editor/tools/material', 'editor/tools/flowforge',
                 'engine/process', 'engine/domain', 'modules']
    assert not git('diff', base, impl, '--', *protected)
    spec = read(here / 'spec-input.json')
    assert digest(archived(spec['archive_path']).read_bytes()) == spec['sha256']
    with zipfile.ZipFile(archived(spec['archive_path'])) as package:
        for name in package.namelist():
            if not name.endswith('/'): assert package.read(name) == (here / 'spec' / name).read_bytes()
    before = read(here / 'before/baseline.json')
    git('merge-base', '--is-ancestor', before['production_sha'], base)
    assert not git('diff', before['production_sha'], base, '--', 'engine', 'editor/tools/scene/execution/src')
    assert digest((here / 'before/runs.cpp').read_bytes()) == before['test_sha256']
    for command in read(here / 'before/commands.json'):
        assert digest(archived(command['archive_log']).read_bytes()) == command['sha256']
        assert command['exit_code'] == (0 if command['argv'][0] == 'cmake' else 1)
    assert all(c['exit_code'] == 1 for c in read(here / 'before/results.json'))
    for scenario, count in [('queued', 2), ('mixed', 3), ('failed', 1)]:
        old = (here / 'before' / ('r1-' + scenario + '.log')).read_text()
        assert old.count('readable=0') == count and old.count('INVALID_ID') == count
        assert 'stop_complete=1' in old and 'instance_absent=1' in old
        new = (here / 'logs' / ('run-r1-' + scenario + '.log')).read_text()
        assert new.count('readable=1') == count and 'INVALID_ID' not in new
    assert 'late CANCELLED' in (here / 'logs/run-r1-callback.log').read_text()
    assert '64 generations, 32 slots' in (here / 'logs/run-r1-capacity.log').read_text()
    assert 'copied failure code lifetime' in (here / 'logs/runtime-detail.log').read_text()
    assert 'cannot recursively acknowledge' in (here / 'logs/run-r1-failed.log').read_text()
    commands = {PurePosixPath(c['archive_log']).stem: c for c in r['commands']}
    groups = r['consumer_groups']; assert len(groups) == 10
    required = ['tracked-snapshot', 'configure', 'build', 'no-work', 'ctest', 'install', 'test-names',
                'player-configure', 'player-build', 'player-no-work', 'player-ctest', 'player-targets',
                'player-test-names', 'player-headless-imports', 'player-runtime-detail', 'runtime-detail',
                'driver-detail', 'run-imports', 'architecture', 'package-audit', 'clean-final',
                'clone-before-status', 'clone-checkout', 'original-p06-gate', 'original-p05-r2-gate',
                'original-p05-r1-gate', 'historical-verifiers', 'portable', 'persistence-files',
                'persistence-coordinator', 'three-sessions', 'C01', 'C03', 'C04']
    boundaries = ['boundaries', 'model-boundaries', 'material-boundaries', 'flow-boundaries',
                  'persistence-boundaries', 'run-boundaries']
    required += boundaries
    required += [g + '-' + a for g in groups for a in ['configure', 'build', 'no-work', 'ctest']]
    required += ['run-' + s for s in ['isolation', 'controls', 'failure', 'completion',
                                    'r1-queued', 'r1-mixed', 'r1-failed', 'r1-callback', 'r1-capacity']]
    required += ['persistence-' + s for s in ['r1-revoke', 'r1-recursive', 'r1-chain', 'r1-chain-control',
                  'r1-callbacks', 'r1-reading', 'r1-conflicts', 'r2-accept-success', 'r2-accept-error',
                  'r2-accept-cancel', 'r2-accept-drain', 'r2-admission']]
    assert set(required) <= commands.keys(), set(required) - commands.keys()
    for name, c in commands.items():
        assert c['implementation_sha'] == impl and c['exit_code'] == (1 if name in ['C01', 'C03', 'C04'] else 0)
        assert digest(archived(c['archive_log']).read_bytes()) == c['sha256']
    for name in ['configure', 'player-configure']:
        assert '-DLUX_EDITOR_MIGRATION_STAGE=P06' in commands[name]['argv']
    assert '-DLUX_BUILD_PROFILE=PLAYER' in commands['player-configure']['argv']
    for name in ['architecture', 'package-audit']:
        argv = commands[name]['argv']; assert argv[argv.index('--stage') + 1] == 'P06'
        assert not read(archived(commands[name]['archive_log']))['findings']
    for name in ['build', 'no-work', 'player-build', 'player-no-work']:
        argv = commands[name]['argv']; assert argv[argv.index('--target') + 1] == 'all'
        assert argv[argv.index('-j') + 1] == '4' and argv[-2:] == ['-k', '0']
    for name in ['no-work', 'player-no-work'] + [g + '-no-work' for g in groups]:
        assert 'ninja: no work to do' in archived(commands[name]['archive_log']).read_text()
    for name in ['ctest', 'player-ctest'] + [g + '-ctest' for g in groups]:
        assert '100% tests passed, 0 tests failed' in archived(commands[name]['archive_log']).read_text()
    assert not archived(commands['clean-final']['archive_log']).read_bytes()
    assert not archived(commands['clone-before-status']['archive_log']).read_bytes()
    assert commands['clone-checkout']['argv'][-1] == impl
    prior = read(root / 'dev_log/P06/test-coverage.json')['final_tests']
    old_names = {x['name'] for x in prior}; assert len(old_names) == 121
    names = {x['name'] for x in read(here / 'logs/test-names.log')['tests']}
    assert old_names <= names
    assert {'editor.scene_execution.r1-' + s for s in ['queued', 'mixed', 'failed', 'callback', 'capacity']} <= names
    player = {x['name'] for x in read(here / 'logs/player-test-names.log')['tests']}
    assert player == {x['name'] for x in read(root / 'dev_log/P06/logs/player-test-names.log')['tests']}
    assert not any(n.startswith('editor.') for n in player)
    assert '/editor/' not in (here / 'evidence/player-target-directories.log').read_text().replace('\\', '/').lower()
    for unit in read(here / 'evidence/player-compile-commands.json'):
        assert '/editor/' not in (unit['file'] + unit['command']).replace('\\', '/').lower()
    for name in boundaries:
        cases = read(here / 'evidence' / (name + '.json'))
        prior_cases = read(root / 'dev_log/P06/evidence' / (name + '.json'))
        assert {c['id'] for c in cases} == {c['id'] for c in prior_cases}
        assert cases and all(c['passed'] and c['graph'] and c['repaired_exit_code'] == 0 for c in cases)
        assert all(c['exit_code'] != 0 and c['expected_rule'] for c in cases if 'legal' not in c['id'])
    for path in ['editor/tools/scene/execution/test/runs.cpp', 'engine/scene/composition/test/runtime.cpp']:
        old = assertions(git('show', base + ':' + path).decode())
        new = assertions(git('show', impl + ':' + path).decode())
        assert not old - new, (path, old - new)
    source = git('show', impl + ':engine/scene/composition/src/SceneRuntime.cpp').decode()
    record = source[source.index('struct Record final'):source.index('struct TimerReceiver final')]
    assert not re.search(r'std::array<Step|struct Step|next_step\{', record)
    assert source.count('std::array<Step, 32> steps') == 1 and source.count('next_step{1}') == 1
    header = git('show', impl + ':engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp').decode()
    assert not re.search(r'\b(?:valid|invalid|getSceneRegistry|getClock|tick|destroy)\s*\(|\bTickResult\b', header)
    run = git('show', impl + ':editor/tools/scene/execution/src/RunStore.cpp').decode()
    assert not re.search(r'\bdriveFrame\s*\(', run)
    for name, marker in [('C01', 'visible_before=0 visible_after=1'), ('C03', 'released_during_query=1'),
                         ('C04', 'create_succeeded=1 outcome_succeeded=0')]:
        assert r['known_failures'][name]['status'] == 'FAIL'
        assert marker in (here / 'logs' / (name + '.log')).read_text()
    assert {x['id'] for x in r['test_results']} == {f'R06-R1-{i:02}' for i in range(1, 6)}
    assert all(x['status'] == 'PASS' for x in r['test_results'])
    for name in ['architecture', 'migration-ledger', 'test-coverage']:
        d = read(here / (name + '.json'))
        assert d['implementation_sha'] == impl and d['p06_r1']['status'] == 'PASS'
    probes = read(here / 'evidence/receipt-portability.json')
    assert {p['case'] for p in probes} == {'producer-unavailable', 'missing-result', 'corrupt-result'}
    assert all(p['matched'] for p in probes)
    print('PASS P06 R1: real before/after, bounded retained results, old assertions, PLAYER, SDK and dependency negatives')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(); check(args.repo.resolve())
