"""Freeze final P12 qualification; original snapshots and user checkout remain read-only."""
from pathlib import Path
from datetime import datetime,timezone
import json,subprocess,hashlib,shutil,gzip
w=Path(__file__).resolve().parent;s=Path('E:/SyncForder/CodeRepos/lux-engine-p12');o=s.with_name('lux-engine')
git=lambda *args:subprocess.check_output(['git',*args],cwd=s)
sha=git('rev-parse','HEAD').decode().strip();base='0a6644d8781dd3aeea1da549a87dfd6303a1c1dc'
f=w/'final'/sha;build=s.parent/'build/RelWithDebInfo'/('p12-'+sha[:12]);a=s/'dev_log/P12'
assert not a.exists() and not git('status','--porcelain').strip()
assert not git('diff','--name-only',base,sha,'--','dev_log').strip()
baseline=json.loads((w/'baseline.json').read_text())
assert hashlib.sha256((o/'editor/project/src/ProjectBuilder.cpp').read_bytes()).hexdigest()==baseline['user_sha256']
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=o,text=True).strip()==baseline['original_head']
assert subprocess.check_output(['git','rev-parse','main'],cwd=o,text=True).strip()==baseline['main']
a.mkdir(parents=True);(a/'.gitattributes').write_bytes(b'* -text\n')
def write(name,data):
 p=a/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n',encoding='utf8')
def copy(src,dest):
 p=a/dest;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,p)
def tree(src,dest):
 for p in sorted(src.rglob('*')):
  if p.is_file() and '__pycache__' not in p.parts:copy(p,Path(dest)/p.relative_to(src))
records=json.loads((f/'commands.json').read_text())
for r in records:
 assert r['implementation_sha']==sha
 assert hashlib.sha256((f/r['log']).read_bytes()).hexdigest()==r['sha256']
 copy(f/r['log'],'logs/'+r['log']);r['log']='logs/'+r['log']
for p in f.iterdir():
 if p.is_file() and p.name!='commands.json':copy(p,'logs/'+p.name)
for folder,prefix,dest in [('sdk','','sdk'),('abi-independence','abi-','abi'),('public-headers','public-','public-headers')]:
 tree(f/folder,'evidence/'+dest)
 for r in json.loads((f/folder/'commands.json').read_text()):
  r['name']=prefix+r['name'];r['log']='evidence/'+dest+'/'+r['log'];r['implementation_sha']=sha;records.append(r)
tree(f/'installed-product','evidence/installed-product')
tree(f/'installed-menu','evidence/installed-menu')
tree(f/'developer-product','evidence/developer-product')
assert len({r['name'] for r in records})==len(records)
groups=sorted({r['name'].removesuffix('-configure') for r in records if r['log'].startswith('evidence/sdk/') and r['name'].endswith('-configure')})
assert len(groups)==24
for group in groups:
 for step in ['configure','build','no-work','ctest']:assert next(r for r in records if r['name']==group+'-'+step)['exit_code']==0
for name in ['build','no-work','ctest','native-input','sdk-native-input','cpu-ctest','player-ctest','sdk-all','clang-public-headers','regenerate-no-work','installed-editor-smoke','installed-menu','developer-install','developer-editor-smoke']:
 assert next(r for r in records if r['name']==name)['exit_code']==0,name
for folder in ['input','runs','protected','path-regression','sdk-candidate','plugin-startup-probe']:tree(w/folder,folder)
for attempt in (w/'final').iterdir():
 if attempt.is_dir() and attempt!=f:tree(attempt,'failures/qualification-'+attempt.name)
for p in w.glob('*.py'):copy(p,'scripts/'+p.name)
copy(w/'verify_archive.py','verify.py')
for name in ['baseline.json','behavior-map.json','removal-plan.json','g-semantic-map.json','g-original-assertions.json','g-moves.json','tests-before.json','regression-name-map.json','headers-g-sync.json','headers-final-check.json','source-summary.json']:
 copy(w/name,name)
copy(build/'editor-architecture/targets.json','dependency-map.json');copy(build/'CMakeCache.txt','evidence/CMakeCache.txt')
copy(build/'install_manifest.txt','evidence/install_manifest.txt');copy(s/'editor/tests/architecture/rules.json','evidence/rules.json')
with gzip.GzipFile(filename=str(a/'evidence/compile_commands.json.gz'),mode='wb',mtime=0) as stream:stream.write((build/'compile_commands.json').read_bytes())
copy(w.parent/'migration-ledger.json','ledger-snapshot.json')
files=[]
for p in git('diff','--no-renames','--name-only',base,sha).decode().splitlines():
 r=subprocess.run(['git','show',sha+':'+p],cwd=s,capture_output=True)
 files.append({'path':p,'deleted':bool(r.returncode),'sha256':None if r.returncode else hashlib.sha256(r.stdout).hexdigest()})
write('files.json',files)
# Anchor source preservation in Git; semantic migrations refer to exact original blobs.
changed_tests=[]
for p in git('diff','--no-renames','--name-only',base,sha).decode().splitlines():
 if '/test/' in p or '/tests/' in p or p.startswith('cmake/installed-consumers/'):
  blobs={}
  for label,ref in [('before',base),('after',sha)]:
   r=subprocess.run(['git','rev-parse',ref+':'+p],cwd=s,capture_output=True,text=True);blobs[label]=None if r.returncode else r.stdout.strip()
  changed_tests.append({'path':p,**blobs})
write('changed-test-sources.json',changed_tests)
receipt=dict(phase='P12',migration_stage='P12',layering_mode='STRICT',status='PARTIAL_USER_WAIVER',input_sha=base,implementation_sha=sha,
 frozen_at=datetime.now(timezone.utc).isoformat(),stop_after='P12',continuation_authorized=False,commands=records,consumer_groups=groups,
 worktrees={'implementation':str(s),'original':str(o),'original_head':baseline['original_head'],'main':baseline['main'],'user_patch_applied':False,'user_file_sha256':baseline['user_sha256'],'relocated_patch':'protected/relocated.patch'},
 platform_scope={'windows':'AUTOMATED_CHECKS_PASS_REMAINING_ACCEPTANCE_WAIVED','linux':'NOT_RUN','system_ime':'NOT_RUN','sanitizer':'NOT_RUN','old_50k':'Original PARTIAL unchanged; not rerun'},
 defects={x:{'current_result':'PASS','historical_result':'Original FAIL snapshots unchanged','test':'editor.application' if x!='C03' else 'editor.commands'} for x in ['C01','C03','C04']},
 ownership='One SessionStore, SaveService/WriteCoordinator, RunStore/SceneRuntime, original ExecutionRuntime and ViewHost. Application composes their short scopes and owning results; detached views never own author content or accepted background operations.',
 historical_records='All prior dev_log snapshots remain byte-identical. The abandoned becc qualification and every development failure are retained with their actual cause.',
 qualifications='Original behavior source/assertion/name mappings retained across old protocol deletion; counts are not the semantic criterion. Fresh installed sole product, V7 DLL, real IO, CPU, PLAYER, explicit GPU/native input, dependency/operation/concept negatives.',
 notes='Nine old roots and hidden bridges removed. Formal AssetImporter, ProjectStorage, Run editing, ProjectBuilder and readonly legacy data migration retained. P13 is not authorized.')
receipt['user_waiver']=json.loads((w/'user-waiver.json').read_text(encoding='utf8'))
receipt['final_acceptance_verifier']='NOT_RUN_BY_USER_REQUEST; strict PASS requirements unchanged'
copy(w/'user-waiver.json','user-waiver.json')
write('receipt.json',receipt)
copy(w/'report.md','README.md')
copy(w/'installation-cleanup/result.json','evidence/installation-cleanup.json')
copy(w/'installation-cleanup/final-scan.json','evidence/installation-final-scan.json')
write('artifacts.json',[{'path':p.relative_to(a).as_posix(),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(a.rglob('*')) if p.is_file() and p.name!='artifacts.json'])
print('Frozen',sha,'commands',len(records),'groups',len(groups))
