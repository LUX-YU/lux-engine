"""P10 archive gate. Evidence is resolved only inside dev_log; source is read at its implementation SHA."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, re, subprocess, zipfile


def check(root, *, verify_portability=True):
    here = root / 'dev_log/P10'
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def git(*args): return subprocess.check_output(['git', *args], cwd=root)
    def digest(data): return hashlib.sha256(data).hexdigest()
    def archived(name):
        path = PurePosixPath(name)
        assert not path.is_absolute() and '..' not in path.parts and ':' not in name and '\\' not in name
        result = root.joinpath(*path.parts).resolve()
        assert result.is_relative_to(root / 'dev_log')
        return result
    r = read(here / 'receipt.json')
    impl, base = r['implementation_sha'], r['input_sha']
    assert base == '3363f83dcc448db9e79cea97c5176a546c05cf5a'
    assert r['phase'] == r['migration_stage'] == r['stop_after'] == 'P10'
    assert r['status'] == 'PASS' and not r['continuation_authorized']
    git('merge-base', '--is-ancestor', base, impl)
    git('merge-base', '--is-ancestor', impl, 'HEAD')
    for item in read(here / 'artifacts.json'):
        assert digest(archived(item['archive_path']).read_bytes()) == item['sha256'], item
    changes = set(git('diff', '--no-renames', '--name-only', base, impl).decode().splitlines())
    files = read(here / 'files.json')
    assert {x['path'] for x in files} == changes
    assert 'editor/project/src/ProjectBuilder.cpp' not in changes
    assert not git('diff', base, impl, '--', 'dev_log', 'editor/workspace', 'editor/persistence', 'editor/editing/history', 'editor/editing/sessions', 'engine/process')
    for entry in files:
        if entry['change'] == 'D':
            assert subprocess.run(['git','cat-file','-e',impl+':'+entry['path']],cwd=root,capture_output=True).returncode
        else:
            assert digest(git('show',impl+':'+entry['path'])) == entry['git_content_sha256']
    # Unchanged test bodies stay byte-for-byte intact. Migrated bodies keep every assertion;
    # explicit token substitutions describe API relocation, never an assertion removal.
    substitutions = read(here / 'evidence/test-migrations.json')
    for path in git('ls-tree','-r','--name-only',base).decode().splitlines():
        if path.startswith('dev_log/') or not path.endswith(('.cpp','.hpp','.py')):
            continue
        if '/test/' not in path and '/tests/' not in path:
            continue
        before, after = git('show',base+':'+path), git('show',impl+':'+path)
        if before == after:
            continue
        row = substitutions[path]
        if row['kind'] == 'api_relocation':
            text = before.decode()
            for old, new in row['substitutions']:
                text = text.replace(old,new)
            assert re.sub(r'\s+','',text) == re.sub(r'\s+','',after.decode()), path
        else:
            assert path.startswith('editor/tests/architecture/')
            assert row['kind'] == 'additive_boundary_fixtures'
            assert digest(before) == row['before_sha256'] and digest(after) == row['after_sha256']
    spec = read(here / 'spec-input.json')
    with zipfile.ZipFile(archived(spec['archive_path'])) as package:
        assert digest(archived(spec['archive_path']).read_bytes()) == spec['sha256']
        for name in package.namelist():
            if not name.endswith('/'):
                assert package.read(name) == (here/'spec'/name).read_bytes()
    commands = {PurePosixPath(c['archive_log']).stem:c for c in r['commands']}
    previous = read(root/'dev_log/P09-R1/receipt.json')
    inherited = {PurePosixPath(c['archive_log']).stem for c in previous['commands']}
    required = inherited | {'desktop-boundaries','desktop-detail','desktop-imports','original-p09-r1-gate','gpu-device'}
    groups = r['consumer_groups']
    assert groups[:-1] == previous['consumer_groups'] and groups[-1] == 'desktop-views-p10-consumer'
    required.update(g+'-'+a for g in groups for a in ['configure','build','no-work','ctest'])
    assert required <= commands.keys(), required-commands.keys()
    def log(name): return archived(commands[name]['archive_log']).read_text(errors='replace')
    for name, c in commands.items():
        assert c['implementation_sha'] == impl
        assert c['exit_code'] == (1 if name in ['C01','C03','C04','after-sdk-illegal-runtime'] else 0), name
        assert digest(archived(c['archive_log']).read_bytes()) == c['sha256']
    for name in ['configure','player-configure']:
        assert '-DLUX_EDITOR_MIGRATION_STAGE=P10' in commands[name]['argv']
    for name in ['architecture','package-audit','desktop-boundaries']:
        argv = commands[name]['argv']; assert argv[argv.index('--stage')+1] == 'P10'
    for name in ['architecture','package-audit']:
        assert not json.loads(log(name))['findings']
    for name in ['build','no-work','player-build','player-no-work']:
        argv = commands[name]['argv']
        assert argv[argv.index('--target')+1] == 'all' and argv[argv.index('-j')+1] == '4' and argv[-2:] == ['-k','0']
    for name in ['no-work','player-no-work']+[g+'-no-work' for g in groups]:
        assert 'ninja: no work to do' in log(name), name
    for name in ['ctest','player-ctest']+[g+'-ctest' for g in groups]:
        assert '100% tests passed, 0 tests failed' in log(name), name
    assert not log('clean-final').strip() and not log('clone-before-status').strip()
    assert commands['clone-checkout']['argv'][-1] == impl
    prior = {x['name'] for x in read(root/'dev_log/P09-R1/logs/test-names.log')['tests']}
    current = {x['name'] for x in read(here/'logs/test-names.log')['tests']}
    assert len(prior) == 180 and prior < current
    assert current-prior == {'editor.view_host','editor.scene_views_gpu','editor.desktop_native_input','editor.desktop_view_boundaries','editor.project_views'}
    assert len(read(here/'logs/player-test-names.log')['tests']) == 11
    assert '-DLUX_BUILD_PROFILE=PLAYER' in commands['player-configure']['argv']
    for unit in read(here/'evidence/player-compile-commands.json'):
        assert '/editor/' not in (unit['file']+' '+unit['command']).replace('\\','/').lower()
    total = sum(int(re.findall(r'out of (\d+)',log(g+'-ctest'))[-1]) for g in groups)
    assert total == r['test_totals']['sdk'] and total >= 97
    for path in (here/'evidence').glob('*boundaries.json'):
        cases=read(path); assert cases and all(c.get('passed') for c in cases), path
    assert len(read(here/'evidence/desktop-boundaries.json')) == 25
    for unit in read(here/'evidence/desktop-consumer-compile-commands.json'):
        command=unit['command'].replace('\\','/').lower()
        assert '/pinclude/' not in command and '/sinclude/' not in command
    negative=list((here/'logs').glob('reject_*.log')); assert len(negative) == 8
    for path in negative:
        data=path.read_text(errors='replace'); assert 'C2280' in data and 'C1083' not in data
    for mode in ['GPU_UI','EDITOR_SCENE_PANE']:
        prefix='explicit-'+mode.lower()
        assert '-DCONSUMER_MODE='+mode in commands[prefix+'-configure']['argv']
        assert '100% tests passed' in log(prefix+'-ctest') and 'ninja: no work to do' in log(prefix+'-no-work')
    for name in ['desktop-detail','desktop-views-p10-consumer-ctest']:
        text=log(name)
        assert 'validation_errors=0' in text and 'native desktop' in text and 'system IME candidate/commit NOT tested' in text
        assert 'left/right highlight isolation and retirement verified' in text
        assert 'catalog BUSY/IO/permission preserves rows' in text
    assert 'exact author encoding' in log('desktop-views-p10-consumer-ctest')
    assert {x['id'] for x in r['test_results']} == {f'X10-{i:02}' for i in range(1,8)}
    assert {x['id'] for x in r['related_q']} == {'Q02','Q03','Q04','Q21','Q26','Q29','Q34','Q50','Q51'}
    assert r['input_scope']['system_ime'] == 'NOT_RUN'
    for id, marker in [('C01','visible_before=0 visible_after=1'),('C03','released_during_query=1'),('C04','create_succeeded=1 outcome_succeeded=0')]:
        assert marker in log(id) and r['known_failures'][id]['status'] == 'FAIL'
    assert r['user_change']['included'] is False
    assert r['user_change']['sha256'] == 'ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'
    for name in ['architecture','migration-ledger','test-coverage']:
        value=read(here/(name+'.json'))
        assert value['current_phase'] == 'P10' and value['p10']['implementation_sha'] == impl
    if verify_portability:
        probes=read(here/'evidence/receipt-portability.json')
        assert {p['case'] for p in probes} == {'producer-unavailable','missing-result','corrupt-result'}
        assert all(p['matched'] for p in probes)
    print('PASS P10: exact implementation, formal desktop/tools, real dual SceneView GPU/native input, installed generator, inherited regressions; system IME NOT_RUN')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2])
    check(parser.parse_args().repo.resolve())
