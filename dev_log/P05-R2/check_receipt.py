"""P05 R2 gate: archive-relative evidence, historical code verified at implementation_sha."""
import argparse
import hashlib
import json
import re
from pathlib import Path, PurePosixPath
import subprocess
import sys
import zipfile


def check(root):
    here = root / 'dev_log/P05-R2'
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
    base = '5a1ccbb47d2d08ffa8cfda907f74319c4a694068'
    assert receipt['phase'] == 'P05-R2' and receipt['migration_stage'] == 'P05' and receipt['status'] == 'PASS'
    assert receipt['input_sha'] == base and receipt['stop_after'] == 'P05' and not receipt['continuation_authorized']
    git('merge-base', '--is-ancestor', base, impl)
    git('merge-base', '--is-ancestor', impl, 'HEAD')
    predecessors = ['P00','P01','P01-R1','P02','P02-R1','P03','P03-R1','P04','P04-R1','P05','P05-R1']
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
        for suffix, local in [('REVIEW_AND_INSTRUCTIONS.md','spec/REVIEW_AND_INSTRUCTIONS.md')]:
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
    scenarios = ['models','order','close','close-key','capacity','execution','decoded','receipts','identities','conflict','measure','save-as','unknown','r1-revoke','r1-recursive','r1-chain','r1-chain-control','r1-callbacks','r1-reading','r1-conflicts','r2-accept-success','r2-accept-error','r2-accept-cancel','r2-accept-drain','r2-admission']
    required = ['tracked-snapshot','configure','build','no-work','ctest','install','architecture','package-audit',
        'clone-origin','clone-fetch','clone-before-status','clone-checkout','clean-final','test-names','original-p05-gate','original-p05-r1-gate',
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
    for name in ['configure']:
        assert '-DLUX_EDITOR_MIGRATION_STAGE=P05' in commands[name]['argv']
    for name in ['architecture','package-audit']:
        argv = commands[name]['argv']
        assert argv[argv.index('--stage')+1] == 'P05'
        assert not read(archived(commands[name]['archive_log']))['findings']
    for name in ['no-work'] + [g+'-no-work' for g in groups]:
        assert 'ninja: no work to do' in archived(commands[name]['archive_log']).read_text()
    for name in ['ctest'] + [g+'-ctest' for g in groups]:
        assert '100% tests passed, 0 tests failed' in archived(commands[name]['archive_log']).read_text()
    old_names = {x['name'] for x in read(root/'dev_log/P05-R1/test-coverage.json')['final_tests']}
    names = {x['name'] for x in read(here/'logs/test-names.log')['tests']}
    assert len(old_names) == 111 and old_names <= names
    assert {'editor.persistence.'+s for s in scenarios} | {'editor.persistence.real_files','editor.persistence.write_coordinator','editor.persistence_boundaries'} <= names
    # Existing assertions remain byte-identical, not merely replaced by an equivalent test count.
    old_test_files = [p for p in git('ls-tree','-r','--name-only',base).decode().splitlines()
        if p.endswith(('.cpp','.hpp','.py')) and ('/test/' in p or '/tests/' in p) and not p.startswith('dev_log/')]
    permitted = {'editor/tests/persistence/models.cpp'}
    for path in old_test_files:
        original = git('show',base+':'+path)
        current = git('show',impl+':'+path)
        if path not in permitted:
            assert original == current, path
        else:
            # All original lines (including every assertion) remain in their original order.
            lines = iter(current.decode().splitlines())
            assert all(any(x == old for x in lines) for old in original.decode().splitlines()), path
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
    assert {f'X05-{n:02}' for n in range(1,10)} | {f'R05-{n:02}' for n in range(1,9)} | {f'R05-R2-{n:02}' for n in range(1,6)} <= results.keys()
    assert all(x['status']=='PASS' for x in results.values())
    for name in ['migration-ledger','architecture','test-coverage']:
        record = read(here/(name+'.json'))
        assert record['current_phase']=='P05' and record['implementation_sha']==impl and record['p05_r2']['status']=='PASS'
    ledger = read(here/'migration-ledger.json')
    assert all(x['delete_by']=='P12' and x['consumers'] for x in ledger['p05_retained_adapters'])
    for name, marker in [('C01','visible_before=0 visible_after=1'),('C03','released_during_query=1'),('C04','create_succeeded=1 outcome_succeeded=0')]:
        assert marker in archived(commands[name]['archive_log']).read_text()
        assert receipt['known_failures'][name]['status']=='FAIL'
    protected = ['dev_log/'+p for p in predecessors] + ['editor/app/test/baseline_failures.cpp',
        'editor/app/src/EditorTestAccess.cpp','editor/transition','editor/editing/history','editor/editing/sessions','editor/history','editor/sessions','editor/tools/scene/model','editor/tools/material/model','editor/tools/flowforge/model','engine','modules','editor/project','editor/persistence/src/WriteCoordinator.cpp','editor/persistence/include/lux/engine/editor/persistence/WriteCoordinator.hpp']
    assert not git('diff',base,impl,'--',*protected)
    imports = archived(commands['persistence-imports']['archive_log']).read_text().lower()
    assert not any(x in imports for x in ['vulkan','editor_app.dll','editor_context.dll','editor_ui.dll','editor_scene.dll',
        'editor_material.dll','editor_flowforge.dll','scene_runtime.dll','scene_composition.dll'])
    assert all(x['matched'] for x in read(here/'logs/portability/results.json'))
    assert all(x['exit_code']==0 for x in read(here/'logs/historical/results.json'))
    assert not archived(commands['clean-final']['archive_log']).read_bytes()
    assert not archived(commands['clone-before-status']['archive_log']).read_bytes()
    assert commands['clone-checkout']['argv'][-1] == impl
    for name in ['build','no-work']:
        argv = commands[name]['argv']
        assert argv[argv.index('--target')+1] == 'all' and argv[argv.index('-j')+1] == '4'
    before = {x['case']:x for x in read(here/'before/results.json')}
    for mode in ['success','error','cancel']:
        item=before['r2-accept-'+mode]
        assert item['exit_code'] == 20
        log=(here/'before'/item['log']).read_bytes()
        assert digest(log)==item['sha256']
        for marker in ['collected=1 encoding_calls=1 stuck_encoding=1', 'still_encoding=1 reserved=1',
                       'follower_ready=1 follower_blocked=1 final_disk=material1']:
            assert marker.encode() in log
    provenance=read(here/'before/provenance.json')
    assert provenance['base_sha']==base and provenance['sdk']
    assert provenance['user_file_sha256']=='ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'
    link_inputs=(here/'before/consumer-link.txt').read_text()
    assert '@CMakeFiles\\persistence_consumer.rsp' in link_inputs
    # The original command uses Ninja's response file. Retain its SDK hashes independently;
    # the final unchanged link declaration is expanded from the archived installed-consumer rule.
    link_inputs += (here/'evidence/consumer-build.ninja').read_text()
    declarations = [git('show', sha+':cmake/installed-consumers/persistence/CMakeLists.txt').decode() for sha in [base, impl]]
    assert re.findall(r'target_link_libraries\([^)]*\)', declarations[0]) == re.findall(r'target_link_libraries\([^)]*\)', declarations[1])
    sdk_names = {x['path'].replace('\\','/').split('/')[-1] for x in provenance['sdk']}
    assert {'editor_persistence.lib','project_io.lib'} <= sdk_names
    for library in ['editor_persistence.lib','material_model.lib','project_io.lib']:
        assert library in link_inputs and 'install' in link_inputs
    assert 'test_shim' not in link_inputs
    for marker in ['72', 'callback', 'P05']:
        assert marker in (here/'README.md').read_text()
    for mode in ['success','error','cancel','drain']:
        log=archived(commands['persistence-r2-accept-'+mode]['archive_log']).read_text()
        assert 'collected=1 encoding_calls=1 stuck_encoding=0' in log
    admission=archived(commands['persistence-r2-admission']['archive_log']).read_text()
    assert admission.count('12 bounded cycles:')==6
    assert 'task-capacity rejection settles EXECUTION' in admission
    cold=receipt['cold_build_observation']
    assert cold['status']=='UNFIXED_EXISTING_BUILD_ORDER_RISK'
    assert archived(cold['evidence']).is_file()
    assert 'R05-05' in archived(commands['persistence-coordinator']['archive_log']).read_text()
    assert 'R05-08' in archived(commands['persistence-coordinator']['archive_log']).read_text()
    assert all(x['status']=='FAIL' for x in receipt['known_failures'].values())
    print('PASS P05 R2 gate: X05-01..09; three real models, IO/adoption/rebind and stale completion; original 111 tests/assertions; R05-R2-01..05; '
          'R05-01..08; clean-clone exact-SHA all/build/install/9 consumers/negative closure; C01/C03/C04 remain FAIL; stop at P05.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try: check(args.evidence_root.resolve())
    except FileNotFoundError as error:
        print('EVIDENCE_INCOMPLETE: '+str(error.filename),file=sys.stderr);raise SystemExit(1)
    except (AssertionError,KeyError,ValueError,OSError,subprocess.CalledProcessError) as error:
        print(str(error) or 'EVIDENCE_INVALID',file=sys.stderr);raise SystemExit(1)
