"""Portable P11 qualification: archived evidence and immutable Git objects only."""
from pathlib import Path
import argparse,hashlib,json,subprocess,re

def check(source,archive):
 archive=archive.resolve()
 def load(name): return json.loads((archive/name).read_text(encoding='utf-8'))
 receipt=load('receipt.json');assert receipt['phase']==receipt['migration_stage']==receipt['stop_after']=='P11'
 assert receipt['layering_mode']=='STRICT' and receipt['status']=='PASS'
 assert receipt['continuation_authorized'] is False
 base=receipt['input_sha'];sha=receipt['implementation_sha']
 assert base=='22ab1a30862f6bff943cef86d4232839173c7107'
 def git(*args):return subprocess.check_output(['git',*args],cwd=source)
 git('merge-base','--is-ancestor',base,sha);git('merge-base','--is-ancestor',sha,'HEAD')
 assert not git('diff','--name-only',base,sha,'--','dev_log').strip()
 assert git('show',base+':editor/authoring/project/src/ProjectBuilder.cpp')==git('show',sha+':editor/authoring/project/src/ProjectBuilder.cpp')
 artifacts=load('artifacts.json');mapping={x['path']:x['sha256'] for x in artifacts};assert len(mapping)==len(artifacts)>100
 def data(name):
  p=(archive/name).resolve();assert p.is_relative_to(archive) and p.is_file(),name
  b=p.read_bytes();assert hashlib.sha256(b).hexdigest()==mapping[name],name
  return b
 for name in mapping:data(name)
 files=json.loads(data('files.json'));assert {f['path'] for f in files}==set(git('diff','--name-only',base,sha).decode().splitlines())
 for f in files:
  r=subprocess.run(['git','show',sha+':'+f['path']],cwd=source,capture_output=True)
  assert bool(r.returncode)==f['deleted'],f['path']
  if not f['deleted']:assert hashlib.sha256(r.stdout).hexdigest()==f['sha256'],f['path']
 regression=json.loads(data('regression-source-map.json'))
 for item in regression['unchanged']:
  assert git('rev-parse',base+':'+item['path']).decode().strip()==item['blob']
  assert git('rev-parse',sha+':'+item['path']).decode().strip()==item['blob']
 for item in regression['changed']:
  assert git('rev-parse',base+':'+item['path']).decode().strip()==item['before']
  assert git('rev-parse',sha+':'+item['path']).decode().strip()==item['after']
  assert regression['changed_test_rationale'].get(item['path']),item['path']
 records=receipt['commands'];commands={r['name']:r for r in records};assert len(commands)==len(records)
 for c in records:
  assert c['implementation_sha']==sha and c['started']<=c['ended']
  assert c['sha256']==mapping[c['log']];data(c['log'])
 def passed(name):
  c=commands[name];assert c['exit_code']==0,name;return c
 for name in ['tracked-snapshot','configure','build','no-work','ctest','install','test-names',
              'cpu-configure','cpu-build','cpu-no-work','cpu-ctest','player-configure','player-build',
              'player-no-work','player-ctest','clang-public-headers','clang-build','clang-ctest',
              'regenerate','regenerate-no-work','abi-before-configure','abi-after-editor-version-configure']:
  passed(name)
 for name in ['configure','cpu-configure','player-configure']:
  assert '-DLUX_EDITOR_MIGRATION_STAGE=P11' in commands[name]['argv']
  assert '-DLUX_EDITOR_LAYERING_MODE=STRICT' in commands[name]['argv']
 for name in ['build','cpu-build','player-build','regenerate']:
  args=commands[name]['argv'];assert args[args.index('--target')+1]=='all' and args[args.index('-j')+1]=='4'
  assert args[-2:]==['-k','0']
 for name in ['no-work','cpu-no-work','player-no-work','regenerate-no-work']:
  assert b'ninja: no work to do' in data(commands[name]['log'])
 current=json.loads(data(passed('test-names')['log']));old=json.loads(data('tests-before.json'))
 assert {t['name'] for t in old['tests']} <= {t['name'] for t in current['tests']}
 for name in ['editor.commands','editor.contributions','editor.sessions.installation','editor.extensions.boundaries']:
  assert name in {t['name'] for t in current['tests']}
 details=data('logs/ctest-details.log')
 for name, observation in [('editor.scene_views_gpu',b'left/right highlight isolation and retirement verified'),
                           ('editor.desktop_native_input',b'OS mouse drag -> generated Inspector preview/commit/undo')]:
  block=re.search(rb'\d+/\d+ Testing: '+re.escape(name.encode())+rb'\r?\n.*?time elapsed:.*?\n',details,re.S)
  assert block and b'Test Passed.' in block[0] and b'validation_errors=0' in block[0] and observation in block[0],name
 for group in receipt['consumer_groups']:
  for step in ['configure','build','no-work','ctest']:passed(group+'-'+step)
  consumer_log=data('evidence/sdk/'+group+'-ctest-details.log')
  assert b'Testing:' in consumer_log and b'Test Passed.' in consumer_log,group
 for domain in ['scene','material','flowforge']:passed('persistence-'+domain+'-isolated')
 assert len(receipt['consumer_groups'])==24
 assert {'p11','p11-runtime','p11-models','gpu-ui','editor-scene-pane'} <= set(receipt['consumer_groups'])
 for group in ['gpu-ui','editor-scene-pane']:
  assert b'Test Passed.' in data('evidence/sdk/'+group+'-ctest-details.log'),group
 assert b'PASS installed RenderFeature: three GPU frames' in data('evidence/sdk/gpu-ui-ctest-details.log')
 assert b'PASS Presentation/RenderFeature through SceneRuntime:' in data('evidence/sdk/editor-scene-pane-ctest-details.log')
 assert b'PASS 10000 shared-entry revisions retain one value without an owner chain' in details
 for defect,expected in [('C01',1),('C03',0),('C04',1)]:
  assert commands[defect]['exit_code']==expected
 assert receipt['defects']['C03']['current_result']=='PASS'
 assert receipt['worktrees']['user_patch_applied'] is False
 assert receipt['platform_scope']['linux']=='NOT_RUN' and receipt['platform_scope']['system_ime']=='NOT_RUN'
 abi=json.loads(data('evidence/abi/result.json'));assert abi['implementation_sha']==sha
 assert abi['runtime_identity_unchanged'] and abi['editor_identity_changed']
 assert abi['runtime_before']==abi['runtime_after'] and abi['editor_before']!=abi['editor_after']
 for item in json.loads(data('source-map.json')):
  assert item['symbol_scope'] and item['replacement_step'] and item['providers']
  assert item['action'] in ['MOVED_ORIGINAL_DELETED','RETAINED_PRODUCT_OR_TEST']
  if item['action']=='RETAINED_PRODUCT_OR_TEST':assert item['deadline']=='P12' and item['last_product_consumer']
  else:
   assert subprocess.run(['git','cat-file','-e',sha+':'+item['path']],cwd=source,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode!=0
 residual=json.loads(data('retained-file-inventory.json'))
 assert residual['implementation_sha']==sha
 for item in residual['files']:
  assert item['providers'] and item['replacement'] and item['deadline']=='P12'
  assert git('rev-parse',sha+':'+item['path']).decode().strip()==item['blob']
 assert {x['id'] for x in json.loads(data('behavior-map.json'))}=={*['X11-'+str(n).zfill(2) for n in range(1,8)],'X11-A','X11-B','X11-C','X11-D'}
 print('PASS P11 archive: actual evidence, source objects, stage, original behavior, SDK, GUI and bounded owners')

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--archive',type=Path,required=True);a=p.parse_args()
 check(a.source.resolve(),a.archive)
