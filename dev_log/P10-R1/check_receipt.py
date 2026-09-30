"""P10 R1 archive gate: source at implementation SHA; evidence only via relative archive paths."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, re, subprocess, zipfile

def check(root, *, verify_portability=True):
    here=root/'dev_log/P10-R1'
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def digest(data): return hashlib.sha256(data).hexdigest()
    def git(*args): return subprocess.check_output(['git',*args],cwd=root)
    def archived(name):
        p=PurePosixPath(name)
        assert not p.is_absolute() and '..' not in p.parts and ':' not in name and '\\' not in name
        path=root.joinpath(*p.parts).resolve()
        assert path.is_relative_to(root/'dev_log')
        return path
    r=read(here/'receipt.json'); impl=r['implementation_sha']; base=r['input_sha']
    assert base=='b9b856477755a9247a7a6810fa880ae62151f565'
    assert r['phase']==r['migration_stage']==r['stop_after']=='P10' and r['revision']=='R1'
    assert r['status']=='PASS' and not r['continuation_authorized']
    git('merge-base','--is-ancestor',base,impl);git('merge-base','--is-ancestor',impl,'HEAD')
    for item in read(here/'artifacts.json'):
        assert digest(archived(item['archive_path']).read_bytes())==item['sha256'],item
    files=read(here/'files.json'); changes=set(git('diff','--name-only',base,impl).decode().splitlines())
    assert {x['path'] for x in files}==changes
    allowed={'editor/tools/material/ui/src/MaterialView.cpp','editor/tools/flowforge/ui/src/FlowView.cpp',
        'editor/tools/scene/ui/test/views.cpp','editor/tools/scene/ui/test/DraftSources.hpp',
        'editor/tools/scene/ui/CMakeLists.txt','cmake/installed-consumers/desktop-views/CMakeLists.txt',
        'editor/tests/architecture/rules.json'}
    assert changes==allowed
    for row in files: assert digest(git('show',impl+':'+row['path']))==row['git_content_sha256']
    # Existing test body and every original assertion remain in order. New tests are additive.
    for p in changes - {'editor/tools/scene/ui/test/DraftSources.hpp'}:
        if p.startswith('dev_log/') or not p.endswith(('.cpp','.hpp','.py')) or not ('/test/' in p or '/tests/' in p):continue
        before=git('show',base+':'+p);after=git('show',impl+':'+p)
        if before==after:continue
        assert p=='editor/tools/scene/ui/test/views.cpp'
        tokens=iter(re.findall(r'\w+|[^\s]',after.decode()))
        for token in re.findall(r'\w+|[^\s]',before.decode()):assert any(x==token for x in tokens),token
    # Test-only exact include additions must not relax production dependency rules.
    before_rules=json.loads(git('show',base+':editor/tests/architecture/rules.json'))
    after_rules=json.loads(git('show',impl+':editor/tests/architecture/rules.json'))
    def without_test_headers(value):
        if isinstance(value,dict):return {k:without_test_headers(v) for k,v in value.items() if k!='test_headers'}
        if isinstance(value,list):return [without_test_headers(v) for v in value]
        return value
    assert without_test_headers(before_rules)==without_test_headers(after_rules)
    for name in ['material','flowforge']:
        text=git('show',impl+f':editor/tools/{name}/ui/src/'+('MaterialView' if name=='material' else 'FlowView')+'.cpp').decode()
        assert 'ContentStamp based_on;' in text and 'validate(pending.based_on)' in text
        assert not re.search(r'\b(?:draft_base_|properties_base_|edits_)\b',text)
    before=read(here/'before/receipt.json')
    assert before['implementation_sha']=='11de2c1fee9d477daaa2d26b07911306ea9d6b9f' and before['input_sha']==base
    assert {x['argv'][-1] for x in before['commands']}=={'r1-literal','r1-signature','r1-flow-canvas','r1-material-canvas'}
    for c in before['commands']:
        p=here/'before'/c['log'];assert c['exit_code']==1 and digest(p.read_bytes())==c['sha256']
        assert 'source_and_history_preserved=0 stale=0' in p.read_text()
    spec=read(here/'spec-input.json');p=archived(spec['archive_path']);assert digest(p.read_bytes())==spec['sha256']
    with zipfile.ZipFile(p) as z:
        for name in z.namelist():
            if not name.endswith('/'):assert z.read(name)==(here/'spec'/name).read_bytes()
    previous=read(root/'dev_log/P10/receipt.json')
    commands={PurePosixPath(c['archive_log']).stem:c for c in r['commands']}
    inherited={PurePosixPath(c['archive_log']).stem for c in previous['commands']}
    assert inherited|{'draft-source-native','original-p10-gate'}<=commands.keys()
    def log(name):return archived(commands[name]['archive_log']).read_text(errors='replace')
    for name,c in commands.items():
        assert c['implementation_sha']==impl
        assert c['exit_code']==(1 if name in ['C01','C03','C04','after-sdk-illegal-runtime'] else 0),name
        assert digest(archived(c['archive_log']).read_bytes())==c['sha256']
    groups=r['consumer_groups'];assert groups==previous['consumer_groups'] and len(groups)==14
    for name in ['configure','player-configure']:assert '-DLUX_EDITOR_MIGRATION_STAGE=P10' in commands[name]['argv']
    for name in ['architecture','package-audit','desktop-boundaries']:
        args=commands[name]['argv'];assert args[args.index('--stage')+1]=='P10'
    for name in ['architecture','package-audit']:assert not json.loads(log(name))['findings']
    for name in ['build','no-work','player-build','player-no-work']:
        args=commands[name]['argv'];assert args[args.index('--target')+1]=='all' and args[args.index('-j')+1]=='4' and args[-2:]==['-k','0']
    for name in ['no-work','player-no-work']+[g+'-no-work' for g in groups]:assert 'ninja: no work to do' in log(name),name
    for name in ['ctest','player-ctest','draft-source-native']+[g+'-ctest' for g in groups]:assert '100% tests passed, 0 tests failed' in log(name),name
    assert not log('clean-final').strip() and not log('clone-before-status').strip()
    assert commands['clone-checkout']['argv'][-1]==impl
    prior={t['name'] for t in read(root/'dev_log/P10/logs/test-names.log')['tests']}
    current={t['name'] for t in read(here/'logs/test-names.log')['tests']}
    assert len(prior)==185 and prior<=current
    cases={'literal','signature','flow-canvas','material-canvas','flow-queue','material-queue','material-lifetime','lifecycle','positive','history'}
    assert current-prior=={'editor.draft_source_'+x for x in cases}
    assert len(read(here/'logs/player-test-names.log')['tests'])==11
    for unit in read(here/'evidence/player-compile-commands.json'):
        assert '/editor/' not in (unit['file']+' '+unit['command']).replace('\\','/').lower()
    assert '-DLUX_BUILD_PROFILE=PLAYER' in commands['player-configure']['argv']
    total=sum(int(re.findall(r'out of (\d+)',log(g+'-ctest'))[-1]) for g in groups)
    assert total==r['test_totals']['sdk']==106
    native=log('draft-source-native');sdk=log('desktop-views-p10-consumer-ctest')
    for case in cases:assert 'editor.draft_source_'+case in native
    for case in cases-{'history'}:assert 'installed.desktop.draft_source_'+case in sdk
    for marker in ['source_and_history_preserved=1 stale=1','actual Revert/edit/Apply one-history Undo/Redo PASS',
        'BUSY stable pointer/source/stage','dynamic node draft: BUSY retains/code alive/disposal under original gate PASS',
        'failed BUSY rebind preserves; old generation rejected; new binding same IDs recaptured PASS']:
        assert marker in native and marker in sdk,marker
    for p in (here/'evidence').glob('*boundaries.json'):
        cases=read(p);assert cases and all(c['passed'] for c in cases),p
    assert len(read(here/'evidence/desktop-boundaries.json'))==25
    for unit in read(here/'evidence/desktop-consumer-compile-commands.json'):
        command=unit['command'].replace('\\','/').lower();assert '/pinclude/' not in command and '/sinclude/' not in command
    negatives=list((here/'logs').glob('reject_*.log'));assert len(negatives)==8
    for p in negatives:assert 'C2280' in p.read_text(errors='replace') and 'C1083' not in p.read_text(errors='replace')
    for mode in ['GPU_UI','EDITOR_SCENE_PANE']:
        name='explicit-'+mode.lower();assert '-DCONSUMER_MODE='+mode in commands[name+'-configure']['argv']
        assert '100% tests passed' in log(name+'-ctest') and 'ninja: no work to do' in log(name+'-no-work')
    for name in ['desktop-detail','desktop-views-p10-consumer-ctest']:
        for marker in ['validation_errors=0','native desktop','system IME candidate/commit NOT tested',
            'left/right highlight isolation and retirement verified','catalog BUSY/IO/permission preserves rows']:
            assert marker in log(name),marker
    assert {t['id'] for t in r['test_results']}=={f'R10-R1-{i:02}' for i in range(1,7)}
    assert r['input_scope']['system_ime']=='NOT_RUN'
    for id,marker in [('C01','visible_before=0 visible_after=1'),('C03','released_during_query=1'),('C04','create_succeeded=1 outcome_succeeded=0')]:
        assert marker in log(id) and r['known_failures'][id]['status']=='FAIL'
    assert r['user_change']['included'] is False and r['user_change']['sha256']=='ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'
    observation=r['validation_observations'][0]
    assert observation['id']=='workspace-catalog-access-denied' and observation['status']=='RETRY_PASS'
    incident=read(archived(observation['evidence']))
    assert incident['failed_command']['exit_code']==3221226505
    assert incident['failed_command']['implementation_sha']==impl
    assert not incident['production_change'] and not incident['test_change']
    assert digest((here/'development/P10-R1-catalog-crash.dmp').read_bytes())==incident['dump_sha256']
    assert len(incident['retries'])==10 and (here/'development/catalog-failure-files').is_dir()
    for retry in incident['retries']:
        assert retry['argv']==incident['failed_command']['argv'] and retry['exit_code']==0
        assert digest((here/'development'/retry['log']).read_bytes())==retry['sha256']
    assert commands['workspace-catalog']['exit_code']==0
    for name in ['architecture','migration-ledger','test-coverage']:
        value=read(here/(name+'.json'));assert value['current_phase']=='P10' and value['p10_r1']['implementation_sha']==impl
    if verify_portability:
        probes=read(here/'evidence/receipt-portability.json')
        assert {p['case'] for p in probes}=={'producer-unavailable','missing-result','corrupt-result'} and all(p['matched'] for p in probes)
    print('PASS P10 R1: actual UI provenance negative/control/recovery, original 185, PLAYER, 14 SDK, new/old GPU, P10 gate; IME NOT_RUN')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2]);check(parser.parse_args().repo.resolve())
