"""P09 archive gate: read archived artifacts and versioned Git blobs, never producer-machine files."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, re, subprocess, zipfile


def check(root):
    here = root / 'dev_log/P09'
    def read(p): return json.loads(p.read_text(encoding='utf-8-sig'))
    def git(*args): return subprocess.check_output(['git', *args], cwd=root)
    def digest(data): return hashlib.sha256(data).hexdigest()
    def archived(name):
        p = PurePosixPath(name)
        assert not p.is_absolute() and '..' not in p.parts and ':' not in name and '\\' not in name
        result = root.joinpath(*p.parts).resolve()
        assert result.is_relative_to(root / 'dev_log')
        return result
    r = read(here / 'receipt.json')
    impl, base = r['implementation_sha'], r['input_sha']
    assert base == '41167a9bdff8c192fe990d53aa8dfa132d57b081'
    assert r['phase'] == r['migration_stage'] == r['stop_after'] == 'P09'
    assert r['status'] == 'PASS' and not r['continuation_authorized']
    git('merge-base', '--is-ancestor', base, impl)
    git('merge-base', '--is-ancestor', impl, 'HEAD')
    for entry in read(here / 'artifacts.json'):
        assert digest(archived(entry['archive_path']).read_bytes()) == entry['sha256'], entry
    files = read(here / 'files.json')
    changed = set(git('diff', '--no-renames', '--name-only', base, impl).decode().splitlines())
    assert {x['path'] for x in files} == changed
    for f in files:
        if f['change'] == 'D':
            assert subprocess.run(['git', 'cat-file', '-e', impl + ':' + f['path']], cwd=root, capture_output=True).returncode != 0
        else:
            assert digest(git('show', impl + ':' + f['path'])) == f['git_content_sha256']
    protected = ['dev_log', 'engine', 'modules', 'editor/project', 'editor/tools', 'editor/editing', 'editor/app', 'editor/context']
    assert not git('diff', base, impl, '--', *protected)
    # Preserve every existing C++ behavioral test verbatim, rather than claiming a count is semantic evidence.
    for path in git('ls-tree', '-r', '--name-only', base).decode().splitlines():
        if path.endswith(('.cpp', '.hpp', '.py')) and ('/test/' in path or '/tests/' in path):
            if path.startswith(('editor/tests/architecture/', 'dev_log/')):
                continue  # Architecture policy is explicitly extended and exercises actual negative fixtures below.
            assert git('show', base + ':' + path) == git('show', impl + ':' + path), path
    spec = read(here / 'spec-input.json')
    with zipfile.ZipFile(archived(spec['archive_path'])) as package:
        assert digest(archived(spec['archive_path']).read_bytes()) == spec['sha256']
        for name in package.namelist():
            if not name.endswith('/'):
                assert package.read(name) == (here / 'spec' / name).read_bytes()
    commands = {PurePosixPath(c['archive_log']).stem: c for c in r['commands']}
    old = read(root / 'dev_log/P08-R1/receipt.json')
    groups = r['consumer_groups']
    assert len(groups) == 13 and set(old['consumer_groups']) < set(groups)
    required = ['tracked-snapshot', 'configure', 'build', 'no-work', 'ctest', 'install', 'test-names',
        'player-configure', 'player-build', 'player-no-work', 'player-ctest', 'player-targets', 'player-test-names',
        'player-headless-imports', 'player-runtime-detail', 'runtime-detail', 'driver-detail', 'architecture',
        'package-audit', 'clean-final', 'clone-before-status', 'clone-checkout', 'portable', 'historical-verifiers',
        'persistence-files', 'persistence-coordinator', 'three-sessions', 'C01', 'C03', 'C04',
        'projection-detail', 'highlight-detail', 'backend-binding-detail', 'compilation-detail',
        'ownership-material', 'ownership-flow', 'sdk-owner-detail', 'after-sdk-contract', 'after-sdk-traits',
        'after-sdk-illegal-runtime', 'scene-interaction', 'material-interaction', 'flow-interaction',
        'run-selection', 'detached-views', 'ui-root-detail', 'workspace-effects', 'sdk-workspace-effects', 'workspace-imports']
    boundaries = ['boundaries', 'model-boundaries', 'material-boundaries', 'flow-boundaries',
        'persistence-boundaries', 'run-boundaries', 'projection-boundaries', 'interaction-boundaries', 'workspace-boundaries']
    required += boundaries
    required += ['original-' + stage + '-gate' for stage in ['p03', 'p03-r1', 'p04', 'p04-r1', 'p05', 'p05-r1', 'p05-r2', 'p06', 'p06-r1', 'p07', 'p07-r1', 'p08', 'p08-r1']]
    required += [g + '-' + a for g in groups for a in ['configure', 'build', 'no-work', 'ctest']]
    required += ['explicit-' + m + '-' + a for m in ['gpu_ui', 'editor_scene_pane'] for a in ['configure', 'build', 'no-work', 'ctest']]
    required += ['r1-' + m + '-' + s for m in ['scene', 'material', 'flow'] for s in ['sync', 'cancel', 'selection', 'stale', 'gate', 'thread', 'reload']]
    required += ['sdk-r1-' + m + '-' + s for m in ['scene', 'material', 'flow'] for s in ['sync', 'cancel', 'selection', 'stale', 'gate', 'thread']]
    scenarios = ['validation', 'opaque', 'rename', 'remove', 'ordering', 'unknown', 'aliases', 'reads', 'catalog', 'migration', 'collision', 'budgets']
    required += [prefix + scenario for prefix in ['workspace-', 'sdk-workspace-'] for scenario in scenarios]
    assert set(required) <= commands.keys(), set(required) - commands.keys()
    for name, c in commands.items():
        assert c['implementation_sha'] == impl
        assert c['exit_code'] == (1 if name in ['C01', 'C03', 'C04', 'after-sdk-illegal-runtime'] else 0), name
        assert digest(archived(c['archive_log']).read_bytes()) == c['sha256']
    def log(name): return archived(commands[name]['archive_log']).read_text(errors='replace')
    for name in ['configure', 'player-configure']:
        assert '-DLUX_EDITOR_MIGRATION_STAGE=P09' in commands[name]['argv']
    for name in ['architecture', 'package-audit']:
        argv = commands[name]['argv']
        assert argv[argv.index('--stage') + 1] == 'P09'
        assert not json.loads(log(name))['findings']
    for name in ['build', 'no-work', 'player-build', 'player-no-work']:
        argv = commands[name]['argv']
        assert argv[argv.index('--target') + 1] == 'all'
        assert argv[argv.index('-j') + 1] == '4' and argv[-2:] == ['-k', '0']
    for name in ['no-work', 'player-no-work'] + [g + '-no-work' for g in groups]:
        assert 'ninja: no work to do' in log(name), name
    for name in ['ctest', 'player-ctest'] + [g + '-ctest' for g in groups]:
        assert '100% tests passed, 0 tests failed' in log(name), name
    assert not log('clean-final').strip() and not log('clone-before-status').strip()
    assert commands['clone-checkout']['argv'][-1] == impl
    prior = {x['name'] for x in read(root / 'dev_log/P08-R1/logs/test-names.log')['tests']}
    current = {x['name'] for x in read(here / 'logs/test-names.log')['tests']}
    assert len(prior) == 160 and prior < current
    assert current - prior == {'editor.workspace_boundaries', 'editor.workspace.effects'} | {'editor.workspace.' + x for x in scenarios}
    assert len(read(here / 'logs/player-test-names.log')['tests']) == 11
    assert '-DLUX_BUILD_PROFILE=PLAYER' in commands['player-configure']['argv']
    assert '/editor/' not in (here / 'evidence/player-target-directories.log').read_text().replace('\\', '/').lower()
    for unit in read(here / 'evidence/player-compile-commands.json'):
        assert '/editor/' not in (unit['file'] + ' ' + unit['command']).replace('\\', '/').lower()
    sdk_total = sum(int(re.findall(r'out of (\d+)', log(g + '-ctest'))[-1]) for g in groups)
    assert sdk_total == 87 and r['test_totals']['sdk'] == sdk_total
    for name in boundaries:
        cases = read(here / ('evidence/' + name + '.json'))
        assert cases and all(x.get('passed') for x in cases), name
    workspace_cases = read(here / 'evidence/workspace-boundaries.json')
    assert len(workspace_cases) == 18
    for case in workspace_cases:
        assert case['repaired_exit_code'] == 0 and case['graph']
        if case['expected_rule']:
            assert case['exit_code'] != 0 and case['expected_rule'] in case['log']
    for id, marker in [('C01', 'visible_before=0 visible_after=1'), ('C03', 'released_during_query=1'), ('C04', 'create_succeeded=1 outcome_succeeded=0')]:
        assert marker in log(id)
        assert r['known_failures'][id]['status'] == 'FAIL'
    for suffix in ['', 'sdk-']:
        assert 'native_code=5' in log(suffix + 'workspace-rename')
        assert 'REAL files' in log(suffix + 'workspace-migration')
        assert 'REAL Root + dirty MaterialSession unchanged' in log(suffix + 'workspace-effects')
    assert {x['id'] for x in r['test_results']} == {f'X09-{i:02}' for i in range(1, 7)}
    assert all(x['status'] == 'PASS' for x in r['test_results'])
    assert {x['id'] for x in r['related_q']} == {'Q04', 'Q31', 'Q32', 'Q33', 'Q35', 'Q36', 'Q37', 'Q52'}
    negatives = list((here / 'logs').glob('reject_*.log'))
    assert len(negatives) == 8
    for path in negatives:
        text = path.read_text(errors='replace')
        assert 'C2280' in text and 'C1083' not in text
    for mode in ['GPU_UI', 'EDITOR_SCENE_PANE']:
        prefix = 'explicit-' + mode.lower()
        assert '-DCONSUMER_MODE=' + mode in commands[prefix + '-configure']['argv']
        assert '100% tests passed' in log(prefix + '-ctest')
        assert 'ninja: no work to do' in log(prefix + '-no-work')
    assert 'installed.ui_feature' in log('explicit-gpu_ui-ctest')
    assert 'installed.scene_panes' in log('explicit-editor_scene_pane-ctest')
    for run in read(here / 'evidence/projection-measurements.json')['runs']:
        assert run['archive_log'].startswith('dev_log/P09/logs/')
        assert run['exit_code'] == 0 and archived(run['archive_log']).is_file()
    assert r['user_change']['included'] is False
    assert r['user_change']['sha256'] == 'ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'
    assert r['new_bridges'] == []
    for name in ['architecture', 'migration-ledger', 'test-coverage']:
        data = read(here / (name + '.json'))
        assert data['current_phase'] == 'P09' and data['p09']['implementation_sha'] == impl and data['p09']['status'] == 'PASS'
    print('PASS P09: pure values/plans, real coordinated IO/migration, unchanged behavioral baseline, SDK/PLAYER/GPU, archive-only evidence')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    check(parser.parse_args().repo.resolve())
