"""P07 gate: immutable evidence and Git blobs only; no producer-machine paths are opened."""
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
    here = root / 'dev_log/P07-R1'
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def git(*args): return subprocess.check_output(['git', *args], cwd=root)
    def digest(data): return hashlib.sha256(data).hexdigest()
    def archived(name):
        path = PurePosixPath(name)
        assert not path.is_absolute() and '..' not in path.parts and ':' not in name and '\\' not in name
        result = root.joinpath(*path.parts).resolve(); assert result.is_relative_to(root / 'dev_log')
        return result
    r = read(here / 'receipt.json'); impl = r['implementation_sha']; base = r['input_sha']
    assert base == 'f6598e65f9d77620f1374065a76d82e6e6e00d79'
    assert r['phase'] == r['migration_stage'] == r['stop_after'] == 'P07' and r['status'] == 'PASS'
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
        'editor/tools/scene/model','editor/tools/material/model','editor/tools/flowforge/model',
        'editor/tools/scene/execution','engine/process','engine/scene/composition']
    assert not git('diff',base,impl,'--',*protected)
    spec=read(here/'spec-input.json')
    assert digest(archived(spec['archive_path']).read_bytes()) == spec['sha256']
    with zipfile.ZipFile(archived(spec['archive_path'])) as package:
        for name in package.namelist():
            if not name.endswith('/'): assert package.read(name)==(here/'spec'/name).read_bytes()
    commands={PurePosixPath(c['archive_log']).stem:c for c in r['commands']}
    groups=r['consumer_groups']; assert len(groups)==11
    prior_groups=read(root/'dev_log/P06-R1/receipt.json')['consumer_groups']
    assert set(prior_groups)<=set(groups) and 'projection-compilation-p07-consumer' in groups
    boundaries=['boundaries','model-boundaries','material-boundaries','flow-boundaries',
        'persistence-boundaries','run-boundaries','projection-boundaries']
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
    assert set(required)<=commands.keys(),set(required)-commands.keys()
    for name,c in commands.items():
        assert c['implementation_sha']==impl and c['exit_code']==(1 if name in ['C01','C03','C04','after-sdk-illegal-runtime'] else 0)
        assert digest(archived(c['archive_log']).read_bytes())==c['sha256']
    for name in ['configure','player-configure']: assert '-DLUX_EDITOR_MIGRATION_STAGE=P07' in commands[name]['argv']
    assert '-DLUX_BUILD_PROFILE=PLAYER' in commands['player-configure']['argv']
    for name in ['architecture','package-audit']:
        argv=commands[name]['argv'];assert argv[argv.index('--stage')+1]=='P07'
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
    old_names={x['name'] for x in read(root/'dev_log/P07/logs/test-names.log')['tests']}
    names={x['name'] for x in read(here/'logs/test-names.log')['tests']}
    assert len(old_names)==131 and old_names<=names
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
        if name!='projection-boundaries':
            prior_cases=read(root/'dev_log/P06-R1/evidence'/(name+'.json'))
            assert {c['id'] for c in cases}=={c['id'] for c in prior_cases}
        else: assert len(cases)==17
        assert cases and all(c['passed'] and c['graph'] and c['repaired_exit_code']==0 for c in cases)
        assert all(c['exit_code']!=0 and c['expected_rule'] for c in cases if 'legal' not in c['id'])
    for name,marker in [('C01','visible_before=0 visible_after=1'),('C03','released_during_query=1'),('C04','create_succeeded=1 outcome_succeeded=0')]:
        assert r['known_failures'][name]['status']=='FAIL'
        assert marker in (here/'logs'/(name+'.log')).read_text()
    assert {x['id'] for x in r['test_results']}=={f'X07-{i:02}' for i in range(1,8)}
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
        d=read(here/(name+'.json'));assert d['implementation_sha']==impl and d['p07_r1']['status']=='PASS'
    assert all(x['expires']=='P12' for x in r['new_bridges'])
    assert r['user_change']['sha256']=='ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c' and not r['user_change']['included']
    probes=read(here/'evidence/receipt-portability.json')
    assert {p['case'] for p in probes}=={'producer-unavailable','missing-result','corrupt-result'}
    assert all(p['matched'] for p in probes)
    # Before probes use actual SDK headers and real models; declaration shims are only archived input.
    before=here/'before'
    before_commands={x['name']:x for x in read(before/'commands.json')}
    for name,c in before_commands.items():
        assert digest((before/(name+'.log')).read_bytes())==c['sha256']
        assert c['exit_code']==c['expected']
    for kind in ['material','flow']:
        assert before_commands[kind+'-copy']['exit_code']==1
        assert before_commands[kind+'-control']['exit_code']==0
        failed=(before/(kind+'-copy.log')).read_text()
        assert 'worker_finished=1 business_ready_before=0 original_owner_retained=1' in failed
        assert 'delivered=0 ready=0' in failed and 'author_unchanged=1' in failed
        assert 'delivered=1 ready=1' in (before/(kind+'-control.log')).read_text()
    assert 'acknowledge_busy=1 capacity_stuck=1' in (before/'flow-copy.log').read_text()
    assert (before/'traits.log').read_text().count('copy_ctor=1 copy_assign=1 move_ctor=1 move_assign=1')==2
    assert (before/'contract-negative.log').read_text().count('static assertion failed')==8
    assert 'C1083' not in (before/'contract-negative.log').read_text()
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
        installed=before/Path(path).name
        assert installed.read_bytes().replace(b'\r\n',b'\n')==git('show',base+':'+path).replace(b'\r\n',b'\n')
    # Destructor cleanup and both compiler algorithms are identical apart from corrected comments.
    for path in ['editor/tools/material/preview/src/MaterialCompilation.cpp','editor/tools/flowforge/compilation/src/FlowCompilationService.cpp']:
        old=git('show',base+':'+path).replace(b'last public owner',b'unique public owner')
        assert old==git('show',impl+':'+path)
    assert {x['id'] for x in r['r1_results']}=={f'R07-R1-{i:02}' for i in range(1,6)}
    assert all(x['status']=='PASS' for x in r['r1_results'])
    assert r['new_bridges']==[] and r['review']=='R1'
    print('PASS P07 R1: real SDK before failures, deleted special members, unique ownership, original assertions, compilation/IO/backend, PLAYER, SDK and dependency negatives; stop P07')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2])
    args=parser.parse_args();check(args.repo.resolve())
