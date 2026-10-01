"""Freeze the one mutable P10Q record. Never edits prior dev_log phases."""
from pathlib import Path
import hashlib,json,subprocess,shutil

repo=Path(__file__).resolve().parents[2];work=repo/'.internal/editor-redesign'
git=lambda *args:subprocess.check_output(['git',*args],cwd=repo)
sha=git('rev-parse','HEAD').decode().strip();base='b583e7ffe20e7a1ac55c7119d6a13ac337ebb323'
final=work/'P10Q-final'/sha;archive=repo/'dev_log/P10Q'
assert not archive.exists(),'Do not overwrite a frozen archive'
assert hashlib.sha256((repo/'editor/project/src/ProjectBuilder.cpp').read_bytes()).hexdigest()=='ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'
assert git('rev-parse','main').decode().strip()=='2bb33ff1a1f11025cf404e074c0e9b259239d8a4'
archive.mkdir(parents=True)
def copy(src,dest):
    target=archive/dest;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,target)
def tree(src,dest):
    for f in src.rglob('*'):
        if f.is_file() and '__pycache__' not in f.parts:copy(f,Path(dest)/f.relative_to(src))
tree(final,'logs')
tree(work/'P10Q-before','before')
tree(work/'P10Q-input','input')
tree(work/'P10Q-bench','measurement-source')
for f in work.glob('P10Q-*'):
    if f.is_file():copy(f,Path('development')/f.name)
for name in ['qualify_p10q.py','measure_p10q.py','run_sdk_p10q.py','p10q_inventory.py','freeze_p10q.py',
             'extra_p10q.py','summarize_p10q.py','coverage_p10q.py','verify_archive_p10q.py','sync_sdk_p10q.ps1',
             'count_hierarchy_p10q.py','check_headers_p10q.py']:
    copy(work/name,Path('tools')/name)
for prior in (work/'P10Q-final').iterdir():
    if prior.name!=sha:tree(prior,Path('development/cold-attempts')/prior.name)
for name in ['P10Q-sdk-dev','P10Q-sdk-dev-rest','P10Q-sdk-dev-final','P10Q-sdk-dev-final-02']:
    tree(work/name,Path('development')/name)
copy(work/'migration-ledger.json','migration-ledger.json')
copy(work/'P10Q-report.md','README.md')
copy(work/'P10Q-behavior-map.md','behavior-map.md')
for name in ['P10Q-performance.md','P10Q-link-audit.md']:
    if (work/name).exists():copy(work/name,name.removeprefix('P10Q-'))
files=[]
for raw in git('diff','--name-only',base,sha).decode().splitlines():
    result=subprocess.run(['git','show',sha+':'+raw],cwd=repo,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
    files.append({'path':raw,'deleted':result.returncode!=0,'sha256':hashlib.sha256(result.stdout).hexdigest() if result.returncode==0 else None})
(archive/'files.json').write_text(json.dumps(files,indent=2)+'\n')
(archive/'file-moves.tsv').write_bytes(git('diff','--name-status','-M',base,sha))
commands=[]
for path,relative,prefix in [(final/'commands.json',Path('logs'),''),
                             (final/'sdk/commands.json',Path('logs/sdk'),''),
                             (final/'performance/commands.json',Path('logs/performance'),'performance-'),
                             (final/'extra/commands.json',Path('logs/extra'),'extra-'),
                             (final/'query-counts/commands.json',Path('logs/query-counts'),'parent-count-'),
                             (final/'public-headers/commands.json',Path('logs/public-headers'),'parse-')]:
    if not path.exists():continue
    for raw in json.loads(path.read_text()):
        item=dict(raw);item['name']=prefix+item['name'];item['log']=(relative/item['log']).as_posix();item['implementation_sha']=sha
        commands.append(item)
by_name={v['name']:v for v in commands}
def scope(names,reason=''):
    present=[name for name in names if name in by_name]
    success=len(present)==len(names) and all(by_name[n]['exit_code']==0 for n in names)
    return {'status':'PASS' if success else 'PARTIAL','commands':present,'reason':reason or ('' if success else 'Required commands missing or failed; see retained output')}
groups=['sessions','editor-d2','external-feature','scene-ui','views-ui','scene-model','material-model','flowforge-model',
        'persistence','scene-execution','projection-compilation','interaction-views','workspace','desktop-views']
qualification={
 'windows':scope(['configure','build','no-work','ctest','test-names']),
 'linux':{'status':'NOT_RUN','commands':[],'reason':'User selected Windows first; required Linux Editor/PLAYER/IO/Host/toolchain/SDK not run'},
 'clang-cl':scope(['clang-configure','clang-build','clang-no-work','clang-ctest']),
 'cold-build':scope(['tracked-snapshot','configure','build','no-work']),
 'cpu-native':scope(['cpu-configure','cpu-build','cpu-no-work','cpu-ctest','cpu-test-names']),
 'player':scope(['player-configure','player-build','player-no-work','player-ctest','player-test-names','player-imports']),
 'native-input':scope(['desktop-views-ctest','ctest']),
 'new-dual-viewport':scope(['desktop-views-ctest','ctest']),
 'gpu-ui':scope(['gpu-ui-ctest']),
 'editor-scene-pane':scope(['editor-scene-pane-ctest']),
 'operation-negatives':scope(['projection-compilation-configure','projection-compilation-ctest']),
 'dependency-negatives':scope(['ctest','configure']),
 'performance':scope(['performance-'+name for name in ['configure','build','canvas-ids','candidate-paths','canvas','records','owners','bytes']]
                     +['performance-bq1-'+str(n)+'-'+shape for n in [1000,10000,50000] for shape in ['chain','wide','roots','cycle']]
                     +['performance-bq1-alloc-'+str(n)+'-'+shape for n in [1000,10000,50000] for shape in ['chain','wide','roots','cycle']]
                     +[f'performance-{mode}-{n}-{v}' for mode in ['catalog','tasks','tasks-revision'] for n in [1000,10000] for v in [1,2,8]]
                     +[f'parent-count-parent-{n}-{shape}' for n in [1000,10000,50000] for shape in ['chain','wide','roots','cycle']]),
 'sdk':scope([group+'-'+step for group in groups for step in ['configure','build','no-work','ctest']])}
parses=[name for name in by_name if name.startswith('parse-header-')]
qualification['clang-cl']=scope(['clang-configure','clang-build','clang-no-work','clang-ctest','clang-public-headers',*parses])
if not parses:
    qualification['clang-cl']['status']='PARTIAL'
    qualification['clang-cl']['reason']='Changed installed public-header standalone parse has not run'
before_chain=work/'P10Q-before/bq1-50000-chain-02.log'
completed_before_chain=False
for line in before_chain.read_text(errors='replace').splitlines():
    try:
        value=json.loads(line)
        completed_before_chain=value.get('samples',0)>=100 and value.get('warmup',0)>=10
    except (ValueError,AttributeError):pass
if not completed_before_chain:
    qualification['performance']['status']='PARTIAL';qualification['performance']['reason']='User explicitly ended the old 50k-chain long run before 100 measured samples; completed raw iterations retained, no full-sample claim'
coverage=json.loads((work/'P10Q-coverage.json').read_text(encoding='utf-8'))
receipt={'phase':'P10Q','migration_stage':'P10Q','input_sha':base,'implementation_sha':sha,
 'status':'PARTIAL','stop_after':'P10Q','continuation_authorized':False,'commands':commands,
 'qualification':qualification,'coverage':coverage,'consumer_groups':groups,
 'input_scope':{'system_ime':'NOT_RUN','user_manual_input':'User reported normal; separate from automatic native input',
                'asan':'NOT_RUN: consumer-only instrumented STL rejected unsanitized SDK annotate_string mismatch; configure/build failures retained, no suppression'},
 'known_failures':{name:{'status':'FAIL','owner':owner,'log':'logs/'+name+'.log'} for name,owner in [('C01','P09/P12'),('C03','P11'),('C04','P12')]},
 'protected_file':{'path':'editor/project/src/ProjectBuilder.cpp','sha256':'ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'}}
(archive/'receipt.json').write_text(json.dumps(receipt,indent=2,ensure_ascii=False)+'\n')
artifacts=[{'path':p.relative_to(archive).as_posix(),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
           for p in sorted(archive.rglob('*')) if p.is_file() and p.name not in ['artifacts.json','receipt.json']]
(archive/'artifacts.json').write_text(json.dumps(artifacts,indent=2)+'\n')
print('Frozen P10Q archive at',archive,'implementation',sha)
