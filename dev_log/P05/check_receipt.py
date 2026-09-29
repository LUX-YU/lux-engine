"""P05 gate: archive-relative evidence, historical code verified at implementation_sha."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import zipfile


def check(root):
    here = root / 'dev_log/P05'
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def digest(data): return hashlib.sha256(data).hexdigest()
    def archived(name):
        path = PurePosixPath(name)
        assert not path.is_absolute() and '..' not in path.parts and '\\' not in name and ':' not in name
        result = root.joinpath(*path.parts).resolve()
        assert result.is_relative_to(root)
        return result
    def git(*args): return subprocess.check_output(['git', *args], cwd=root)
    receipt = read(here / 'receipt.json')
    impl = receipt['implementation_sha']
    base = 'aad13c594afe33af69fcfba1b44790b4e9ad148d'
    assert receipt['phase'] == 'P05' and receipt['migration_stage'] == 'P05' and receipt['status'] == 'PASS'
    assert receipt['input_sha'] == base and receipt['stop_after'] == 'P05' and not receipt['continuation_authorized']
    git('merge-base', '--is-ancestor', base, impl)
    git('merge-base', '--is-ancestor', impl, 'HEAD')
    predecessors = ['P00','P01','P01-R1','P02','P02-R1','P03','P03-R1','P04','P04-R1']
    assert set(receipt['predecessor_receipts']) == {'dev_log/' + p + '/receipt.json' for p in predecessors}
    for path in receipt['predecessor_receipts']:
        prior = read(archived(path))
        assert prior['status'] == 'PASS'
        git('merge-base', '--is-ancestor', prior['implementation_sha'], base)
    for item in read(here / 'artifacts.json'):
        assert digest(archived(item['archive_path']).read_bytes()) == item['sha256'], item
    spec = read(here / 'spec-input.json')
    assert digest(archived(spec['archive_path']).read_bytes()) == spec['sha256']
    with zipfile.ZipFile(archived(spec['archive_path'])) as package:
        for suffix, local in [('START_P05.md','spec/START_P05.md'), ('P05_persistence.md','spec/P05_persistence.md')]:
            name = next(n for n in package.namelist() if n.endswith(suffix))
            assert package.read(name) == (here / local).read_bytes()
    files = read(here / 'files.json')
    assert {x['path'] for x in files} == set(git('diff','--name-only',base,impl).decode().splitlines())
    for item in files:
        assert digest(git('show', impl + ':' + item['path'])) == item['git_content_sha256']
    commands = {PurePosixPath(c['archive_log']).stem:c for c in receipt['commands']}
    groups = ['editor-sessions-p01-consumer','ui-resources-editor-d2','ui-resources-external-feature',
        'ui-scene-pane-consumer','ui-views-gpu-consumer','scene-model-p02-consumer','material-model-p03-consumer',
        'flowforge-model-p04-consumer','persistence-p05-consumer']
    scenarios = ['models','order','close','close-key','capacity','execution','decoded','receipts','identities','conflict','measure','save-as','unknown']
    required = ['tracked-snapshot','configure','build','no-work','ctest','install','architecture','package-audit',
        'clone','clone-checkout','clone-tracked','clone-configure','clone-architecture','test-names',
        'boundaries','model-boundaries','material-boundaries','flow-boundaries','persistence-boundaries',
        'portable','historical-verifiers','original-p03-gate','original-p03-r1-gate','original-p04-gate','original-p04-r1-gate',
        'persistence-files','persistence-coordinator','persistence-imports','C01','C03','C04',
        'three-sessions','sessions-detail','close-throw','close-contract']
    required += ['persistence-' + s for s in scenarios]
    required += [g + '-' + a for g in groups for a in ['configure','build','no-work','ctest']]
    assert set(required) <= commands.keys(), set(required) - commands.keys()
    for name, command in commands.items():
        assert command['implementation_sha'] == impl
        assert command['exit_code'] == (1 if name in ['C01','C03','C04'] else 0), name
        assert digest(archived(command['archive_log']).read_bytes()) == command['sha256'], name
    for name in ['configure','clone-configure']:
        assert '-DLUX_EDITOR_MIGRATION_STAGE=P05' in commands[name]['argv']
    for name in ['architecture','clone-architecture','package-audit']:
        argv = commands[name]['argv']
        assert argv[argv.index('--stage')+1] == 'P05'
        assert not read(archived(commands[name]['archive_log']))['findings']
    for name in ['no-work'] + [g+'-no-work' for g in groups]:
        assert 'ninja: no work to do' in archived(commands[name]['archive_log']).read_text()
    for name in ['ctest'] + [g+'-ctest' for g in groups]:
        assert '100% tests passed, 0 tests failed' in archived(commands[name]['archive_log']).read_text()
    old_names = {x['name'] for x in read(root/'dev_log/P04-R1/test-coverage.json')['final_tests']}
    names = {x['name'] for x in read(here/'logs/test-names.log')['tests']}
    assert len(old_names) == 88 and old_names <= names
    assert {'editor.persistence.'+s for s in scenarios} | {'editor.persistence.real_files','editor.persistence.write_coordinator','editor.persistence_boundaries'} <= names
    # Existing assertions remain byte-identical, not merely replaced by an equivalent test count.
    old_test_files = [p for p in git('ls-tree','-r','--name-only',base).decode().splitlines()
        if p.endswith(('.cpp','.hpp','.py')) and ('/test/' in p or '/tests/' in p) and not p.startswith('dev_log/')]
    permitted = {'editor/tests/architecture/check_editor_boundaries.py'}
    for path in old_test_files:
        if path not in permitted:
            assert git('show',base+':'+path) == git('show',impl+':'+path), path
    for name in ['boundaries','model-boundaries','material-boundaries','flow-boundaries','persistence-boundaries']:
        cases = read(here/'evidence'/(name+'.json'))
        assert cases and all(c['passed'] and c['graph'] and c['repaired_exit_code']==0 for c in cases)
    cases = {c['id']:c for c in read(here/'evidence/persistence-boundaries.json')}
    assert len(cases)==17
    for name, case in cases.items():
        if not name.endswith('-legal'): assert case['exit_code'] != 0 and case['expected_rule']
    for name in scenarios:
        assert 'PASS' in archived(commands['persistence-'+name]['archive_log']).read_text(), name
    measurement = archived(commands['persistence-measure']['archive_log']).read_text()
    for token in ['saves=72','executable_plain_allocations=','peak_process_working_set=','tickets_after_ack=0']:
        assert token in measurement
    assert 'Actual filesystem access denial' in archived(commands['persistence-files']['archive_log']).read_text()
    results = {x['id']:x for x in receipt['test_results']}
    assert {f'X05-{n:02}' for n in range(1,10)} <= results.keys()
    assert all(x['status']=='PASS' for x in results.values())
    for name in ['migration-ledger','architecture','test-coverage']:
        record = read(here/(name+'.json'))
        assert record['current_phase']=='P05' and record['implementation_sha']==impl and record['p05']['status']=='PASS'
    ledger = read(here/'migration-ledger.json')
    assert all(x['delete_by']=='P12' and x['consumers'] for x in ledger['p05_retained_adapters'])
    for name, marker in [('C01','visible_before=0 visible_after=1'),('C03','released_during_query=1'),('C04','create_succeeded=1 outcome_succeeded=0')]:
        assert marker in archived(commands[name]['archive_log']).read_text()
        assert receipt['known_failures'][name]['status']=='FAIL'
    protected = ['dev_log/'+p for p in predecessors] + ['editor/app/test/baseline_failures.cpp',
        'editor/app/src/EditorTestAccess.cpp','editor/transition','editor/editing/history','editor/history','editor/sessions']
    assert not git('diff',base,impl,'--',*protected)
    imports = archived(commands['persistence-imports']['archive_log']).read_text().lower()
    assert not any(x in imports for x in ['vulkan','editor_app.dll','editor_context.dll','editor_ui.dll','editor_scene.dll',
        'editor_material.dll','editor_flowforge.dll','scene_runtime.dll','scene_composition.dll'])
    assert all(x['matched'] for x in read(here/'logs/portability/results.json'))
    assert all(x['exit_code']==0 for x in read(here/'logs/historical/results.json'))
    print('PASS P05 gate: X05-01..09; three real models, IO/adoption/rebind and stale completion; original 88 tests/assertions; '
          'exact-SHA build/install/consumers/negative closure; C01/C03/C04 remain FAIL; stop at P05.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try: check(args.evidence_root.resolve())
    except FileNotFoundError as error:
        print('EVIDENCE_INCOMPLETE: '+str(error.filename),file=sys.stderr);raise SystemExit(1)
    except (AssertionError,KeyError,ValueError,OSError,subprocess.CalledProcessError) as error:
        print(str(error) or 'EVIDENCE_INVALID',file=sys.stderr);raise SystemExit(1)
