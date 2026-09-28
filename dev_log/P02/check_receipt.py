"""Validate archived P02 evidence; producer paths are provenance only."""
from pathlib import Path, PurePosixPath
import argparse,hashlib,json,subprocess,sys

def check(root):
 here=root/'dev_log/P02'
 def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))
 def archived(name):
  relative=PurePosixPath(name)
  assert not relative.is_absolute() and '..' not in relative.parts and '\\' not in name, name
  result=root.joinpath(*relative.parts).resolve()
  assert result.is_relative_to(root),name
  return result
 def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
 receipt=read(here/'receipt.json');impl=receipt['implementation_sha']
 assert receipt['phase']=='P02' and receipt['status']=='PASS'
 assert receipt['stop_after']=='P02' and not receipt['continuation_authorized']
 for a,b in [(receipt['input_sha'],impl),(impl,'HEAD')]:
  subprocess.run(['git','merge-base','--is-ancestor',a,b],cwd=root,check=True)
 for item in read(here/'artifacts.json'):
  assert digest(archived(item['archive_path']))==item['sha256'], 'EVIDENCE_HASH_MISMATCH: '+item['archive_path']
 for item in read(here/'files.json'):
  if item['status']=='D':
   assert subprocess.run(['git','cat-file','-e',impl+':'+item['path']],cwd=root,capture_output=True).returncode!=0
  else:
   data=subprocess.check_output(['git','show',impl+':'+item['path']],cwd=root)
   assert hashlib.sha256(data).hexdigest()==item['git_content_sha256'],item['path']
 commands={PurePosixPath(c['archive_log']).stem:c for c in receipt['commands']}
 required=['tracked-snapshot','configure','build','no-work','ctest','install','model-content','model-atomic',
  'model-identity','model-changes','model-plugin','close-throw','close-contract','sessions-detail','boundaries',
  'model-boundaries','architecture','portable','historical-r1-verifier','package-audit','test-names','model-imports','sessions-imports',
  'clone','clone-checkout','clone-tracked','clone-configure','clone-architecture','C01','C03','C04']
 assert set(required)<=commands.keys(),set(required)-commands.keys()
 for name,c in commands.items():
  assert c['implementation_sha']==impl and c['exit_code']==(1 if name in ['C01','C03','C04'] else 0),name
  assert digest(archived(c['archive_log']))==c['sha256'],name
 assert '-DLUX_EDITOR_MIGRATION_STAGE=P02' in commands['configure']['argv']
 assert '-DLUX_EDITOR_MIGRATION_STAGE=P02' in commands['clone-configure']['argv']
 for name in ['architecture','clone-architecture','package-audit']:
  argv=commands[name]['argv'];assert argv[argv.index('--stage')+1]=='P02'
  assert not read(archived(commands[name]['archive_log']))['findings']
 assert 'ninja: no work to do' in archived(commands['no-work']['archive_log']).read_text()
 assert '100% tests passed, 0 tests failed' in archived(commands['ctest']['archive_log']).read_text()
 groups=['editor-sessions-p01-consumer','ui-resources-editor-d2','ui-resources-external-feature',
  'ui-scene-pane-consumer','ui-views-gpu-consumer','scene-model-p02-consumer']
 for group in groups:
  for action in ['configure','build','no-work','ctest']:assert group+'-'+action in commands
  assert 'ninja: no work to do' in archived(commands[group+'-no-work']['archive_log']).read_text()
  assert '100% tests passed, 0 tests failed' in archived(commands[group+'-ctest']['archive_log']).read_text()
 assert all(x['matched'] for x in read(here/'logs/portability/results.json'))
 original={x['name'] for x in read(root/'dev_log/P01-R1/test-coverage.json')['final_tests']}
 tests={x['name'] for x in read(here/'test-coverage.json')['final_tests']}
 assert original<=tests and tests-original=={'editor.scene_model.'+x for x in ['content','atomic','identity','changes','plugin']}|{'editor.scene_model_boundaries'}
 for filename in ['boundaries','model-boundaries']:
  cases=read(here/'evidence'/f'{filename}.json');assert cases and all(c['passed'] for c in cases)
  for c in cases:
   assert c['graph'] and c['repaired_exit_code']==0
 model={c['id']:c for c in read(here/'evidence/model-boundaries.json')}
 for name in ['direct-runtime','intermediate-ui','intermediate-context','imported-ui','old-editor-header',
              'old-context-header','old-bridge-header','runtime-header','unknown-library']:
  c=model[name];assert c['exit_code']!=0 and c['expected_rule'] in c['log']
 results={x['id']:x for x in receipt['test_results']}
 for name in [f'X02-{n:02}' for n in range(1,6)]+[f'X01-{n:02}' for n in range(1,7)]+['P01-R1','P02_GATE','CTEST','INSTALLED']:
  assert results[name]['status']=='PASS',name
 for name,marker in [('C01','visible_before=0 visible_after=1'),('C03','released_during_query=1'),
                     ('C04','create_succeeded=1 outcome_succeeded=0')]:
  log=archived(commands[name]['archive_log']).read_text()
  assert marker in log and f'FAIL {name} intended contract' in log and results[name]['status']=='FAIL'
 old=read(root/'dev_log/P01-R1/migration-ledger.json');ledger=read(here/'migration-ledger.json')
 assert ledger['current_phase']=='P02' and ledger['implementation_sha']==impl
 assert ledger['transition_bridges']==old['transition_bridges']
 seed={x['id']:x for x in ledger['seed_members']}
 for index in range(1,29):
  item=seed[f'D{index:04}'];assert item['p02_action'] and item['delete_by']=='P12'
 assert all(x['delete_by']=='P12' and not x['new_model_dependency'] for x in ledger['p02_retained_adapters'])
 protected=['editor/history','editor/contracts','editor/sessions','editor/transition',
  'editor/app/test/baseline_failures.cpp','editor/app/test/EditorTestAccess.cpp','dev_log/P00','dev_log/P01','dev_log/P01-R1']
 assert not subprocess.check_output(['git','diff',receipt['input_sha'],impl,'--',*protected],cwd=root)
 for name in ['model-imports','sessions-imports']:
  text=archived(commands[name]['archive_log']).read_text().lower()
  assert not any(t in text for t in ['vulkan','editor_app.dll','editor_context.dll','editor_ui.dll','project_storage.dll','scene_runtime.dll','scene_composition.dll'])
 # The original P01 inventory/deadline/AST validator reads its immutable archived evidence.
 subprocess.run([sys.executable,root/'dev_log/P01/verify.py','--evidence-root',root],cwd=root,check=True)
 print('PASS P02 gate: real CPU model X02-01..05, scoped Q, retained P01/R1 semantics, dependency negatives, SDK consumers, clean-clone configure. C01/C03/C04 remain FAIL. Stop at P02.')

if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--evidence-root',type=Path,default=Path(__file__).resolve().parents[2])
 args=parser.parse_args()
 try:check(args.evidence_root.resolve())
 except FileNotFoundError as error:
  print('EVIDENCE_INCOMPLETE: '+str(error.filename),file=sys.stderr);raise SystemExit(1)
 except (AssertionError,KeyError,ValueError,OSError,subprocess.CalledProcessError) as error:
  print(str(error) or 'EVIDENCE_INVALID',file=sys.stderr);raise SystemExit(1)
