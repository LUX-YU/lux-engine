"""P08 gate: immutable evidence and Git blobs only; no producer-machine paths are opened."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, subprocess, zipfile



def check(root):
    here = root / 'dev_log/P08'
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def git(*args): return subprocess.check_output(['git', *args], cwd=root)
    def digest(data): return hashlib.sha256(data).hexdigest()
    def archived(name):
        path = PurePosixPath(name)
        assert not path.is_absolute() and '..' not in path.parts and ':' not in name and '\\' not in name
        result = root.joinpath(*path.parts).resolve(); assert result.is_relative_to(root / 'dev_log')
        return result
    r = read(here / 'receipt.json'); impl = r['implementation_sha']; base = r['input_sha']
    assert base == '833d7efb18f8349cda9968ac3e4ea24ff2b54c60'
    assert r['phase'] == r['migration_stage'] == r['stop_after'] == 'P08' and r['status'] == 'PASS'
    assert not r['continuation_authorized']
    git('merge-base', '--is-ancestor', base, impl); git('merge-base', '--is-ancestor', impl, 'HEAD')
    for item in read(here / 'artifacts.json'):
        assert digest(archived(item['archive_path']).read_bytes()) == item['sha256'], item
    files = read(here / 'files.json')
    assert {f['path'] for f in files} == set(git('diff', '--name-only', base, impl).decode().splitlines())
    for f in files:
        if f['change'] == 'D':
            assert subprocess.run(['git','cat-file','-e',impl+':'+f['path']],cwd=root,capture_output=True).returncode != 0
        else: assert digest(git('show', impl + ':' + f['path'])) == f['git_content_sha256']
    # No old stage snapshots, user changes, model/history/save/Run/runtime algorithms were rewritten.
    protected = ['dev_log','editor/project','editor/editing','editor/persistence','editor/adapters',
        'editor/tools/material/model','engine/process']
    assert not git('diff',base,impl,'--',*protected)
    spec=read(here/'spec-input.json')
    assert digest(archived(spec['archive_path']).read_bytes()) == spec['sha256']
    with zipfile.ZipFile(archived(spec['archive_path'])) as package:
        for name in package.namelist():
            if not name.endswith('/'): assert package.read(name)==(here/'spec'/name).read_bytes()
    commands={PurePosixPath(c['archive_log']).stem:c for c in r['commands']}
    groups=r['consumer_groups']; assert len(groups)==12
    prior_groups=read(root/'dev_log/P07-R1/receipt.json')['consumer_groups']
    assert set(prior_groups)<=set(groups) and 'projection-compilation-p07-consumer' in groups
    boundaries=['boundaries','model-boundaries','material-boundaries','flow-boundaries',
        'persistence-boundaries','run-boundaries','projection-boundaries','interaction-boundaries']
    required=['tracked-snapshot','configure','build','no-work','ctest','install','test-names',
        'player-configure','player-build','player-no-work','player-ctest','player-targets',
        'player-test-names','player-headless-imports','player-runtime-detail','runtime-detail',
        'driver-detail','run-imports','architecture','package-audit','clean-final',
        'clone-before-status','clone-checkout','original-p06-gate','original-p06-r1-gate',
        'original-p05-gate','original-p05-r1-gate','original-p05-r2-gate',
        'original-p04-gate','original-p04-r1-gate','original-p03-gate','original-p03-r1-gate',
        'historical-verifiers','portable','persistence-files','persistence-coordinator',
        'three-sessions','C01','C03','C04','projection-detail','highlight-detail','backend-binding-detail',
        'compilation-detail','compilation-imports','projection-measure']+boundaries
    required += [g+'-'+a for g in groups for a in ['configure','build','no-work','ctest']]
    required += ['original-p07-gate','ownership-material','ownership-flow','sdk-owner-detail','after-sdk-contract','after-sdk-traits','after-sdk-illegal-runtime']
    required += ['run-'+s for s in ['isolation','controls','failure','completion','r1-queued','r1-mixed','r1-failed','r1-callback','r1-capacity']]
    required += ['persistence-'+s for s in ['r1-revoke','r1-recursive','r1-chain','r1-chain-control','r1-callbacks','r1-reading','r1-conflicts','r2-accept-success','r2-accept-error','r2-accept-cancel','r2-accept-drain','r2-admission']]
    required += ['original-p07-r1-gate','scene-interaction','material-interaction','flow-interaction','run-selection','detached-views','ui-root-detail','module-header-sync']
    assert set(required)<=commands.keys(),set(required)-commands.keys()
    for name,c in commands.items():
        assert c['implementation_sha']==impl and c['exit_code']==(1 if name in ['C01','C03','C04','after-sdk-illegal-runtime'] else 0)
        assert digest(archived(c['archive_log']).read_bytes())==c['sha256']
    for name in ['configure','player-configure']: assert '-DLUX_EDITOR_MIGRATION_STAGE=P08' in commands[name]['argv']
    assert '-DLUX_BUILD_PROFILE=PLAYER' in commands['player-configure']['argv']
    for name in ['architecture','package-audit']:
        argv=commands[name]['argv'];assert argv[argv.index('--stage')+1]=='P08'
        assert not read(archived(commands[name]['archive_log']))['findings']
    for name in ['build','no-work','player-build','player-no-work']:
        argv=commands[name]['argv'];assert argv[argv.index('--target')+1]=='all'
        assert argv[argv.index('-j')+1]=='4' and argv[-2:]==['-k','0']
    for name in ['no-work','player-no-work']+[g+'-no-work' for g in groups]:
        assert 'ninja: no work to do' in archived(commands[name]['archive_log']).read_text()
    for name in ['ctest','player-ctest']+[g+'-ctest' for g in groups]:
        assert '100% tests passed, 0 tests failed' in archived(commands[name]['archive_log']).read_text()
    assert not archived(commands['clean-final']['archive_log']).read_bytes()
    assert not archived(commands['clone-before-status']['archive_log']).read_bytes()
    assert commands['clone-checkout']['argv'][-1]==impl
    old_names={x['name'] for x in read(root/'dev_log/P07-R1/logs/test-names.log')['tests']}
    names={x['name'] for x in read(here/'logs/test-names.log')['tests']}
    assert len(old_names)==133 and old_names<=names
    assert {'editor.projection.reset_capacity','editor.projection.highlight_backend',
        'render.features.view_binding','editor.compilation.actual_publish','editor.projection_compilation_boundaries'}<=names
    # Every pre-existing C++ test body is unchanged. Count alone is insufficient.
    for path in git('ls-tree','-r','--name-only',base).decode().splitlines():
        if path.endswith(('.cpp','.hpp')) and any(x in path.split('/') for x in ['test','tests','installed-consumers']):
            assert not git('diff',base,impl,'--',path),path
    player={x['name'] for x in read(here/'logs/player-test-names.log')['tests']}
    assert player=={x['name'] for x in read(root/'dev_log/P06-R1/logs/player-test-names.log')['tests']}
    assert not any(n.startswith('editor.') for n in player)
    assert '/editor/' not in (here/'evidence/player-target-directories.log').read_text().replace('\\','/').lower()
    for unit in read(here/'evidence/player-compile-commands.json'):
        assert '/editor/' not in (unit['file']+unit['command']).replace('\\','/').lower()
    for name in boundaries:
        cases=read(here/'evidence'/(name+'.json'))
        if name not in ['projection-boundaries','interaction-boundaries']:
            prior_cases=read(root/'dev_log/P06-R1/evidence'/(name+'.json'))
            assert {c['id'] for c in cases}=={c['id'] for c in prior_cases}
        elif name=='projection-boundaries': assert len(cases)==17
        else: assert len(cases)==19
        assert cases and all(c['passed'] and c['graph'] and c['repaired_exit_code']==0 for c in cases)
        assert all(c['exit_code']!=0 and c['expected_rule'] for c in cases if 'legal' not in c['id'])
    for name,marker in [('C01','visible_before=0 visible_after=1'),('C03','released_during_query=1'),('C04','create_succeeded=1 outcome_succeeded=0')]:
        assert r['known_failures'][name]['status']=='FAIL'
        assert marker in (here/'logs'/(name+'.log')).read_text()
    assert {x['id'] for x in r['test_results']}=={f'X08-{i:02}' for i in range(1,7)}
    assert all(x['status']=='PASS' and archived(x['archive_log']).is_file() for x in r['test_results'])
    assert 'GPU pixels remain P10/P13' in (here/'logs/backend-binding-detail.log').read_text()
    measurements=read(here/'evidence/projection-measurements.json')
    assert measurements['capacity_contract']['hub_limit']==2
    assert measurements['capacity_contract']['iterations_per_process']==32
    assert len(measurements['runs'])==5
    for sample in measurements['runs']:
        assert sample['exit_code']==0 and sample['elapsed_ns']>0
        assert sample['samples']>0 and sample['peak_working_set_bytes']>0 and sample['sampled_peak_private_bytes']>0
        assert 'X07-07 actual CPU scene' in archived(sample['archive_log']).read_text()
    assert 'real IO and same WriteCoordinator conflict' in (here/'logs/compilation-detail.log').read_text()
    for name in ['architecture','migration-ledger','test-coverage']:
        d=read(here/(name+'.json'));assert d['implementation_sha']==impl and d['p08']['status']=='PASS'
    assert all(x['expires']=='P12' for x in r['new_bridges'])
    assert r['user_change']['sha256']=='ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c' and not r['user_change']['included']
    probes=read(here/'evidence/receipt-portability.json')
    assert {p['case'] for p in probes}=={'producer-unavailable','missing-result','corrupt-result'}
    assert all(p['matched'] for p in probes)
    assert (here/'logs/after-sdk-traits.log').read_text().count('copy_ctor=0 copy_assign=0 move_ctor=0 move_assign=0')==2
    for log in ['after-sdk-illegal-runtime']+['reject_'+kind+'_'+action for kind in ['material','flow'] for action in ['copy_construct','copy_assign','move_construct','move_assign']]:
        text=(here/'logs'/(log+'.log')).read_text()
        assert 'C2280' in text and 'C1083' not in text,log
    headers={'MaterialCompileOperation':'editor/tools/material/preview/include/lux/engine/editor/material/MaterialCompilation.hpp',
             'FlowCompileOperation':'editor/tools/flowforge/compilation/include/lux/engine/editor/flowforge/FlowCompilationService.hpp'}
    for cls,path in headers.items():
        text=git('show',impl+':'+path).decode()
        for signature in [f'{cls}(const {cls}&)',f'operator=(const {cls}&)',f'{cls}({cls}&&)',f'operator=({cls}&&)']:
            assert signature+' = delete;' in text
    # Destructor cleanup and both compiler algorithms are identical apart from corrected comments.
    for path in ['editor/tools/material/preview/src/MaterialCompilation.cpp','editor/tools/flowforge/compilation/src/FlowCompilationService.cpp']:
        old=git('show',base+':'+path).replace(b'last public owner',b'unique public owner')
        assert old==git('show',impl+':'+path)
    assert {row['id'] for row in r['related_q']}=={'Q02','Q10','Q26','Q29','Q34','Q44','Q45'}
    assert r['new_bridges']==[]
    names={x['name'] for x in read(here/'logs/test-names.log')['tests']}
    assert {'editor.scene_interaction','editor.material_interaction','editor.flowforge_interaction',
        'editor.interaction_run_identity','editor.detached_views','editor.interaction_view_boundaries'} <= names
    for path in ['editor/tools/material/preview/include/lux/engine/editor/material/MaterialCompilation.hpp',
        'editor/tools/flowforge/compilation/include/lux/engine/editor/flowforge/FlowCompilationService.hpp']:
        assert not git('diff',base,impl,'--',path)
    for name in ['scene-interaction','material-interaction','flow-interaction']:
        assert 'PASS P08' in (here/'logs'/(name+'.log')).read_text()
    assert 'PASS X08-03..06' in (here/'logs/detached-views.log').read_text()
    assert 'PASS X08-02 real author/Run' in (here/'logs/run-selection.log').read_text()
    synced=read(here/'logs/module-header-sync.log')
    assert len(synced)==15
    for row in synced:
        assert row['copied'] and row['sha256']==digest(git('show',impl+':'+row['source']).replace(b'\r\n',b'\n'))
    print('PASS P08: three independent interactions, real detached UI, move-only owners, P07 assertions, PLAYER, SDK and dependency negatives; stop P08')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2])
    args=parser.parse_args();check(args.repo.resolve())
