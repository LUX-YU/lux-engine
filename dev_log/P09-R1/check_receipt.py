"""P09 R1 archive gate: read archived artifacts and versioned Git blobs, never producer-machine files."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, re, subprocess, zipfile


def check(root):
    here = root / 'dev_log/P09-R1'
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
    assert base == '8bfadc34d73713bf453b45d090874bbb5d8dd399'
    assert r['phase'] == 'P09-R1' and r['migration_stage'] == r['stop_after'] == 'P09'
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
    assert changed == {'editor/workspace/storage/src/LegacyWorkspaceImporter.cpp', 'editor/workspace/README.md',
        'editor/tests/workspace/workspace.cpp', 'editor/tests/workspace/CMakeLists.txt',
        'cmake/installed-consumers/workspace/CMakeLists.txt'}
    protected = ['dev_log', 'engine', 'modules', 'editor/project', 'editor/tools', 'editor/editing', 'editor/app', 'editor/context', 'editor/persistence', 'editor/adapters', 'editor/workspace/layout', 'editor/workspace/recovery']
    assert not git('diff', base, impl, '--', *protected)
    # Preserve every existing C++ behavioral test verbatim, rather than claiming a count is semantic evidence.
    for path in git('ls-tree', '-r', '--name-only', base).decode().splitlines():
        if path.endswith(('.cpp', '.hpp', '.py')) and ('/test/' in path or '/tests/' in path):
            if path.startswith('dev_log/'):
                continue  # Historical archives are protected above; current test sources must remain intact.
            old_bytes, new_bytes = git('show', base + ':' + path), git('show', impl + ':' + path)
            if path == 'editor/tests/workspace/workspace.cpp':
                for start, end in [(b'    void validation()', b'    void budgets()'), (b'    void budgets()', b'int main(')]:
                    assert old_bytes[old_bytes.index(start):old_bytes.index(end)] in new_bytes
                assert old_bytes[old_bytes.index(b'    if (scenario == \"validation\")'):] in new_bytes
            else:
                assert old_bytes == new_bytes, path
    spec = read(here / 'spec-input.json')
    with zipfile.ZipFile(archived(spec['archive_path'])) as package:
        assert digest(archived(spec['archive_path']).read_bytes()) == spec['sha256']
        for name in package.namelist():
            if not name.endswith('/'):
                assert package.read(name) == (here / 'spec' / name).read_bytes()
    commands = {PurePosixPath(c['archive_log']).stem: c for c in r['commands']}
    old = read(root / 'dev_log/P09/receipt.json')
    groups = r['consumer_groups']
    assert len(groups) == 13 and groups == old['consumer_groups']
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
    required += ['original-' + stage + '-gate' for stage in ['p09', 'p03', 'p03-r1', 'p04', 'p04-r1', 'p05', 'p05-r1', 'p05-r2', 'p06', 'p06-r1', 'p07', 'p07-r1', 'p08', 'p08-r1']]
    required += [g + '-' + a for g in groups for a in ['configure', 'build', 'no-work', 'ctest']]
    required += ['explicit-' + m + '-' + a for m in ['gpu_ui', 'editor_scene_pane'] for a in ['configure', 'build', 'no-work', 'ctest']]
    required += ['r1-' + m + '-' + s for m in ['scene', 'material', 'flow'] for s in ['sync', 'cancel', 'selection', 'stale', 'gate', 'thread', 'reload']]
    required += ['sdk-r1-' + m + '-' + s for m in ['scene', 'material', 'flow'] for s in ['sync', 'cancel', 'selection', 'stale', 'gate', 'thread']]
    scenarios = ['validation', 'opaque', 'rename', 'remove', 'ordering', 'unknown', 'aliases', 'reads', 'catalog', 'migration', 'collision', 'budgets']
    review_scenarios = ['r1-alpha','r1-beta','r1-extras','r1-selection','r1-resume','r1-control']
    required += [prefix + scenario for prefix in ['workspace-', 'sdk-workspace-'] for scenario in scenarios + review_scenarios]
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
    prior = {x['name'] for x in read(root / 'dev_log/P09/logs/test-names.log')['tests']}
    current = {x['name'] for x in read(here / 'logs/test-names.log')['tests']}
    assert len(prior) == 174 and prior < current and len(current) == 180
    assert current - prior == {'editor.workspace.' + x for x in review_scenarios}
    assert len(read(here / 'logs/player-test-names.log')['tests']) == 11
    assert '-DLUX_BUILD_PROFILE=PLAYER' in commands['player-configure']['argv']
    assert '/editor/' not in (here / 'evidence/player-target-directories.log').read_text().replace('\\', '/').lower()
    for unit in read(here / 'evidence/player-compile-commands.json'):
        assert '/editor/' not in (unit['file'] + ' ' + unit['command']).replace('\\', '/').lower()
    sdk_total = sum(int(re.findall(r'out of (\d+)', log(g + '-ctest'))[-1]) for g in groups)
    assert sdk_total == 93 and r['test_totals']['sdk'] == sdk_total
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
        assert run['archive_log'].startswith('dev_log/P09-R1/logs/')
        assert run['exit_code'] == 0 and archived(run['archive_log']).is_file()
    assert r['user_change']['included'] is False
    assert r['user_change']['sha256'] == 'ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'
    assert r['new_bridges'] == []
    for name in ['architecture', 'migration-ledger', 'test-coverage']:
        data = read(here / (name + '.json'))
        assert data['current_phase'] == 'P09-R1' and data['p09_r1']['implementation_sha'] == impl and data['p09_r1']['status'] == 'PASS'
    check_before(root, here, r, git)
    print('PASS P09 R1: pure values/plans, real coordinated IO/migration, unchanged behavioral baseline, SDK/PLAYER/GPU, archive-only evidence')


def check_before(root, here, receipt, git):
    before = here / 'before'
    rows = json.loads((before / 'commands.json').read_text())
    commands = {r['name']:r for r in rows}
    assert set(commands) == {'configure','build','no-work','r1-alpha','r1-beta','r1-extras','r1-selection','r1-resume','r1-control'}
    for name, row in commands.items():
        assert row['implementation_sha'] == '38c88aeaf14ca27687d1baf844b4cc2d28cfcfe1'
        assert row['exit_code'] == (1 if name in {'r1-alpha','r1-beta','r1-extras','r1-selection','r1-resume'} else 0)
        data = (root / row['archive_log']).read_bytes()
        assert hashlib.sha256(data).hexdigest() == row['sha256']
    assert b'legacy recovery binding' in (before/'r1-alpha.log').read_bytes()
    assert b'legacy recovery binding' in (before/'r1-beta.log').read_bytes()
    assert b'recovery_entries=2 selected_scope=0' in (before/'r1-extras.log').read_bytes()
    for name in ['r1-alpha','r1-beta','r1-extras']:
        text=(before/(name+'.log')).read_text()
        assert 'individual Alpha accepted=1' in text and 'individual Beta accepted=1' in text
        assert len(re.findall(r'sha256=[0-9a-f]{64}',text)) == 3
        source = before / 'files' / name
        if name == 'r1-beta': source /= 'forward'
        for filename, expected in re.findall(r'input (\S+) sha256=([0-9a-f]{64})', text):
            folder = source / '.lux/editor'
            if filename != 'settings.toml': folder /= 'layouts'
            assert hashlib.sha256((folder/filename).read_bytes()).hexdigest() == expected
    assert (before/'workspace.cpp').read_bytes().replace(b'\r\n', b'\n') == git('show',receipt['implementation_sha']+':editor/tests/workspace/workspace.cpp')
    assert (before/'LegacyWorkspaceImporter.cpp').read_bytes() == git('show',receipt['input_sha']+':editor/workspace/storage/src/LegacyWorkspaceImporter.cpp')
    src=git('show',receipt['implementation_sha']+':editor/workspace/storage/src/LegacyWorkspaceImporter.cpp')
    old=(before/'LegacyWorkspaceImporter.cpp').read_bytes()
    marker=b'    WorkspaceResult<std::optional<persistence::WriteTicket>> WorkspaceStore::continueMigration'
    assert src[src.index(marker):] == old[old.index(marker):] # Publication/marker protocol untouched.
    assert b'legacy recovery binding' not in src and b'if (is_selected)' in src
    assert {r['id'] for r in receipt['review_results']} == {f'R09-R1-{i:02}' for i in range(1,7)}
    assert all(r['status']=='PASS' for r in receipt['review_results'])
    for prefix in ['', 'sdk-']:
        for name in ['r1-alpha','r1-beta','r1-extras','r1-control']:
            text=(here/('logs/'+prefix+'workspace-'+name+'.log')).read_text()
            assert 'selected_scope=1' in text and 'REAL files:' in text
        assert 'selected=Beta' in (here/('logs/'+prefix+'workspace-r1-beta.log')).read_text()
        assert 'empty/absent/missing selection diagnosed' in (here/('logs/'+prefix+'workspace-r1-selection.log')).read_text()
        assert 'marker idempotent, collision rejected PASS' in (here/('logs/'+prefix+'workspace-r1-resume.log')).read_text()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    check(parser.parse_args().repo.resolve())
