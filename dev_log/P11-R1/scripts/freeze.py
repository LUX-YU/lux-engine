"""Freeze P11 R1 against one clean implementation; never modify prior receipts."""
from pathlib import Path
from datetime import datetime,timezone
import json,subprocess,hashlib,shutil,gzip
w=Path(__file__).resolve().parent;s=Path('E:/SyncForder/CodeRepos/lux-engine-p11');o=s.with_name('lux-engine')
git=lambda *args:subprocess.check_output(['git',*args],cwd=s)
sha=git('rev-parse','HEAD').decode().strip();base='24896eaa89ea426c29b4c92daa0f4a0c3bba6fe3'
f=w/'final'/sha;build=s.parent/'build/RelWithDebInfo'/('p11-'+sha[:12]);a=s/'dev_log/P11-R1'
assert not a.exists() and not git('status','--porcelain').strip()
assert not git('diff','--name-only',base,sha,'--','dev_log').strip()
baseline=json.loads((w/'baseline.json').read_text())
assert hashlib.sha256((o/'editor/project/src/ProjectBuilder.cpp').read_bytes()).hexdigest()==baseline['user_sha256']
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=o,text=True).strip()==baseline['original_head']
assert subprocess.check_output(['git','rev-parse','main'],cwd=o,text=True).strip()==baseline['main']
a.mkdir(parents=True);(a/'.gitattributes').write_bytes(b'* -text\n')
def write(name,data):
 p=a/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
def copy(src,dest):
 p=a/dest;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,p)
def tree(src,dest):
 for p in sorted(src.rglob('*')):
  if p.is_file():copy(p,Path(dest)/p.relative_to(src))
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
assert len({r['name'] for r in records})==len(records)
groups=sorted({r['name'].removesuffix('-configure') for r in records if r['log'].startswith('evidence/sdk/') and r['name'].endswith('-configure')})
assert len(groups)==24
for group in groups:
 for step in ['configure','build','no-work','ctest']:assert next(r for r in records if r['name']==group+'-'+step)['exit_code']==0
for name in ['build','no-work','ctest','cpu-ctest','player-ctest','sdk-all','clang-public-headers','regenerate-no-work']:
 assert next(r for r in records if r['name']==name)['exit_code']==0,name
for folder in ['input','before','runs']:tree(w/folder,folder)
for p in w.glob('*.py'):copy(p,'scripts/'+p.name)
copy(w/'verify_archive.py','verify.py');copy(w/'baseline.json','baseline.json');copy(w/'plan.md','plan.md');copy(w/'assertion-preservation.json','assertion-preservation.json')
copy(s/'dev_log/P11/logs/test-names.log','tests-before.json')
copy(build/'editor-architecture/targets.json','dependency-map.json');copy(build/'CMakeCache.txt','evidence/CMakeCache.txt')
copy(build/'install_manifest.txt','evidence/install_manifest.txt');copy(s/'editor/tests/architecture/rules.json','evidence/rules.json')
with gzip.GzipFile(filename=str(a/'evidence/compile_commands.json.gz'),mode='wb',mtime=0) as stream:stream.write((build/'compile_commands.json').read_bytes())
copy(w.parent/'migration-ledger.json','ledger-snapshot.json')
files=[]
for p in git('diff','--name-only',base,sha).decode().splitlines():
 files.append({'path':p,'deleted':False,'sha256':hashlib.sha256(git('show',sha+':'+p)).hexdigest()})
write('files.json',files)
r=json.loads((w/'regression-map.json').read_text());mapped={'unchanged':[], 'changed':[], 'changed_test_rationale':{}}
for item in r['unchanged']:
 assert git('rev-parse',sha+':'+item['path']).decode().strip()==item['before']
 mapped['unchanged'].append({'path':item['path'],'blob':item['before']})
for item in r['modified']:
 mapped['changed'].append({**item,'after':git('rev-parse',sha+':'+item['path']).decode().strip()})
 mapped['changed_test_rationale'][item['path']]='Original behaviors/assertions retained; add real R11 cleanup/batch cases or SDK registration. Original main body remains a separately invoked originalCases.'
# Commands test was extended after the initial test-only negative was frozen.
p='editor/activities/commands/test/commands.cpp'
if not any(x['path']==p for x in mapped['changed']):
 mapped['unchanged']=[x for x in mapped['unchanged'] if x['path']!=p]
 mapped['changed'].append({'path':p,'before':git('rev-parse',base+':'+p).decode().strip(),'after':git('rev-parse',sha+':'+p).decode().strip()})
 mapped['changed_test_rationale'][p]='Keep original pinned/current, query lifetime, 10000 ownership revisions and BUSY cases; add scope move/cleanup, ordinary publication rejection, pinned calls, deferred drain and wrong-thread entry.'
write('regression-source-map.json',mapped)
write('behavior-map.json',[
 {'id':'R11-01','evidence':['editor.persistence.r11-input-duplicate','editor.persistence.r11-input-invalid'],'facts':'Real Material role destructor rejects new business; full encoded source/current/observed/binding/history/dirty unchanged; accurate refusal; one cleanup; later save and undo/redo baseline.'},
 {'id':'R11-02','evidence':['editor.persistence.r11-input-busy','editor.persistence.r11-input-success'],'facts':'Last code release follows owned source destructor; rejected inner entry keeps outer protection; successful prepared role is published and performs real IO.'},
 {'id':'R11-03','evidence':['editor.persistence.r11-input-completion'],'facts':'First collect of finished real SaveExecution inside input cleanup, encoder once, READY retained, new work BUSY, eventual disk/adoption/ack.'},
 {'id':'R11-04','evidence':['editor.contributions.r11-factory','editor.commands'],'facts':'Real detached factory under fixed batch cannot direct-publish, independent registry and pinned query/execute still work; enqueue retained, drain BUSY.'},
 {'id':'R11-05','evidence':['editor.contributions.r11-reflection','editor.commands'],'facts':'Reflection callback cannot publish; rejected configuration leaves real reflection/current catalogs unchanged. Abandoned command candidate destruction remains guarded.'},
 {'id':'R11-06','evidence':['editor.contributions.r11-cleanup','editor.contributions.r11-notify','editor.commands'],'facts':'Both catalogs consistent during cleanup and notifications; nested candidate waits for next outer batch; original 10000 shared-entry regression retained.'},
 {'id':'R11-07','evidence':['editor.sessions.installation','C03','editor.commands'],'facts':'Original real three-model factories, hidden preparation rollback, command target and old/new query checks.'},
 {'id':'R11-08','evidence':['p11-ctest','p11-models-ctest','p11-runtime-ctest','clang-public-headers','abi-after-editor-version-configure'],'facts':'Fresh installed actual V7 DLL both last-owner modes, independent runtime, actual private-header-free consumer, changed headers C++20; runtime ABI unchanged and Editor fingerprint regenerated.'}])
receipt=dict(phase='P11-R1',migration_stage='P11',layering_mode='STRICT',status='PASS',input_sha=base,implementation_sha=sha,
 frozen_at=datetime.now(timezone.utc).isoformat(),stop_after='P11',continuation_authorized=False,commands=records,consumer_groups=groups,
 worktrees={'implementation':str(s),'original':str(o),'original_head':baseline['original_head'],'main':baseline['main'],'user_patch_applied':False,'user_file_sha256':baseline['user_sha256']},
 platform_scope={'windows':'PASS','linux':'NOT_RUN','system_ime':'NOT_RUN','sanitizer':'NOT_RUN','old_50k':'Original PARTIAL unchanged; not rerun'},
 defects={'C01':{'current_result':'FAIL','responsibility':'P12'},'C03':{'current_result':'PASS','historical_result':'FAIL snapshots unchanged'},'C04':{'current_result':'FAIL','responsibility':'P12'}},
 ownership='Existing SaveService dispatch owns admitted rejection cleanup; CommandRegistry grants a stack-bound batch/one-use commit permission; ContributionRegistry composes both scopes. No new service or shared control owner.',
 historical_records='All prior dev_log snapshots unchanged. Original P11 retained-file inventory remains the P12 migration baseline.',
 qualifications='Original 213 named behaviors plus nine R11 cases; actual SDK, DLL, real IO, CPU, PLAYER, explicit GPU/native input, dependency and compile negatives.',
 notes='Counts supplement behavior assertions; package witness is not runtime evidence. Source/test-only before failures and intermediate test-fixture crash retained.')
write('receipt.json',receipt)
copy(w/'report.md','README.md')
write('artifacts.json',[{'path':p.relative_to(a).as_posix(),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(a.rglob('*')) if p.is_file() and p.name!='artifacts.json'])
print('Frozen',sha,'commands',len(records),'groups',len(groups))
