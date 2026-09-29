"""P06 receipt gate. Every evidence read is archive-relative; source is verified at implementation_sha."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, re, subprocess, zipfile

def check(root):
 here=root/'dev_log/P06'
 def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
 def git(*args):return subprocess.check_output(['git',*args],cwd=root)
 def digest(data):return hashlib.sha256(data).hexdigest()
 def archived(name):
  path=PurePosixPath(name)
  assert not path.is_absolute() and '..' not in path.parts and ':' not in name and '\\' not in name
  result=root.joinpath(*path.parts).resolve();assert result.is_relative_to(root)
  return result
 receipt=read(here/'receipt.json');impl=receipt['implementation_sha'];base='fae890a2a848ee68a16ff976e8047ea1b2a3a560'
 assert receipt['phase']=='P06' and receipt['migration_stage']=='P06' and receipt['status']=='PASS'
 assert receipt['input_sha']==base and receipt['stop_after']=='P06' and not receipt['continuation_authorized']
 git('merge-base','--is-ancestor',base,impl);git('merge-base','--is-ancestor',impl,'HEAD')
 for item in read(here/'artifacts.json'):
  assert digest(archived(item['archive_path']).read_bytes())==item['sha256'],item
 for path in receipt['predecessor_receipts']:
  prior=read(archived(path));assert prior['status']=='PASS'
  git('merge-base','--is-ancestor',prior['implementation_sha'],base)
 files=read(here/'files.json')
 assert {i['path'] for i in files}==set(git('diff','--name-only',base,impl).decode().splitlines())
 for item in files:
  assert digest(git('show',impl+':'+item['path']))==item['git_content_sha256']
 spec=read(here/'spec-input.json');assert digest(archived(spec['archive_path']).read_bytes())==spec['sha256']
 with zipfile.ZipFile(archived(spec['archive_path'])) as package:
  for suffix,local in [('START.md','START.md'),('reference/v4/phases/P06_scene_runtime.md','P06_scene_runtime.md')]:
   name=next(n for n in package.namelist() if n.endswith(suffix))
   assert package.read(name)==(here/'spec'/local).read_bytes()
 commands={PurePosixPath(c['archive_log']).stem:c for c in receipt['commands']}
 groups=receipt['consumer_groups'];assert len(groups)==10
 required=['tracked-snapshot','configure','build','no-work','ctest','install','test-names','architecture','package-audit',
  'boundaries','model-boundaries','material-boundaries','flow-boundaries','persistence-boundaries','run-boundaries',
  'player-configure','player-build','player-no-work','player-ctest','player-targets','player-test-names',
  'runtime-detail','driver-detail','player-runtime-detail','player-headless-imports','run-imports',
  'C01','C03','C04','clone-before-status','clone-fetch','clone-checkout','clean-final','portable','historical-verifiers',
  'original-p05-r2-gate','original-p05-r1-gate','original-p05-gate','three-sessions','sessions-detail',
  'persistence-files','persistence-coordinator']
 required += [g+'-'+a for g in groups for a in ['configure','build','no-work','ctest']]
 required += ['run-'+s for s in ['isolation','controls','failure','completion']]
 scenarios=['models','order','close','close-key','capacity','execution','decoded','receipts','identities','conflict','measure','save-as','unknown',
  'r1-revoke','r1-recursive','r1-chain','r1-chain-control','r1-callbacks','r1-reading','r1-conflicts',
  'r2-accept-success','r2-accept-error','r2-accept-cancel','r2-accept-drain','r2-admission']
 required += ['persistence-'+s for s in scenarios]
 assert set(required)<=commands.keys(),set(required)-commands.keys()
 for name,c in commands.items():
  assert c['implementation_sha']==impl and c['exit_code']==(1 if name in ['C01','C03','C04'] else 0),name
  assert digest(archived(c['archive_log']).read_bytes())==c['sha256']
 for name in ['configure','player-configure']:
  assert '-DLUX_EDITOR_MIGRATION_STAGE=P06' in commands[name]['argv']
 assert '-DLUX_BUILD_PROFILE=PLAYER' in commands['player-configure']['argv']
 for name in ['architecture','package-audit']:
  argv=commands[name]['argv'];assert argv[argv.index('--stage')+1]=='P06'
  assert not read(archived(commands[name]['archive_log']))['findings']
 for name in ['no-work','player-no-work']+[g+'-no-work' for g in groups]:
  assert 'ninja: no work to do' in archived(commands[name]['archive_log']).read_text(),name
 for name in ['ctest','player-ctest']+[g+'-ctest' for g in groups]:
  assert '100% tests passed, 0 tests failed' in archived(commands[name]['archive_log']).read_text(),name
 for name in ['build','no-work','player-build','player-no-work']:
  argv=commands[name]['argv'];assert argv[argv.index('--target')+1]=='all' and argv[argv.index('-j')+1]=='4'
 assert not archived(commands['clean-final']['archive_log']).read_bytes()
 assert not archived(commands['clone-before-status']['archive_log']).read_bytes()
 assert commands['clone-checkout']['argv'][-1]==impl
 old={x['name'] for x in read(root/'dev_log/P05-R2/test-coverage.json')['final_tests']}
 names={x['name'] for x in read(here/'logs/test-names.log')['tests']}
 assert len(old)==116 and old<=names
 assert {'editor.scene_execution.'+s for s in ['isolation','controls','failure','completion']}|{'editor.scene_execution_boundaries'}<=names
 player={x['name'] for x in read(here/'logs/player-test-names.log')['tests']}
 assert {'scene.driver','scene.runtime','scene.world_loading','engine.headless_context'}<=player
 assert not any(x.startswith('editor.') for x in player)
 assert '/editor/' not in (here/'evidence/player-target-directories.log').read_text().replace('\\','/').lower()
 for unit in read(here/'evidence/player-compile-commands.json'):
  assert '/editor/' not in (unit['file']+' '+unit['command']).replace('\\','/').lower()
 for name in ['boundaries','model-boundaries','material-boundaries','flow-boundaries','persistence-boundaries','run-boundaries']:
  cases=read(here/'evidence'/(name+'.json'))
  assert cases and all(c['passed'] and c['graph'] and c['repaired_exit_code']==0 for c in cases)
  for case in cases:
   if 'legal' not in case['id']:assert case['exit_code']!=0 and case['expected_rule']
 assert len(read(here/'evidence/run-boundaries.json'))==10
 for item in read(here/'evidence/tests-preserved.json')['unchanged']:
  assert git('rev-parse',base+':'+item['path'])==git('rev-parse',impl+':'+item['path'])
 for item in read(here/'evidence/tests-preserved.json')['changed']:
  assert git('rev-parse',base+':'+item['path']).decode().strip()==item['before']
  assert git('rev-parse',impl+':'+item['path']).decode().strip()==item['after']
 for item in read(here/'evidence/assertion-review.json'):
  assert item['reviewed'] and len(item['mapped'])+len(item['review'])==item['old_count']
  assert all(x['basis'] for x in item['review'])
 protected=['dev_log/P00','dev_log/P01','dev_log/P01-R1','dev_log/P02','dev_log/P02-R1','dev_log/P03','dev_log/P03-R1',
  'dev_log/P04','dev_log/P04-R1','dev_log/P05','dev_log/P05-R1','dev_log/P05-R2',
  'editor/editing/history','editor/editing/sessions','editor/tools/material/model','editor/tools/flowforge/model',
  'editor/persistence','editor/project/src/ProjectBuilder.cpp','editor/app/test/baseline_failures.cpp']
 assert not git('diff',base,impl,'--',*protected)
 header=git('show',impl+':engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp').decode()
 assert not re.search(r'\b(?:valid|invalid|getSceneRegistry|getClock|tick|destroy)\s*\(|\bTickResult\b',header)
 run=git('show',impl+':editor/tools/scene/execution/src/RunStore.cpp').decode()
 assert not re.search(r'\bdriveFrame\s*\(',run)
 for marker in ['32 FIFO','outer guard','S12']:
  assert marker in ''.join((here/'logs'/('run-'+s+'.log')).read_text() for s in ['controls','completion','isolation']) or marker=='32 FIFO' and 'bounded FIFO' in (here/'logs/run-controls.log').read_text()
 assert 'in-flight drain' in (here/'logs/runtime-detail.log').read_text()
 for name,hits in read(here/'evidence/symbol-scan.json').items():
  if name!='retained_private_bridge_consumers':assert not hits,name
 for name,marker in [('C01','visible_before=0 visible_after=1'),('C03','released_during_query=1'),('C04','create_succeeded=1 outcome_succeeded=0')]:
  assert receipt['known_failures'][name]['status']=='FAIL' and marker in (here/'logs'/(name+'.log')).read_text()
 assert all(x['absent_before_build'] for x in read(here/'evidence/physics-generation-inputs.json'))
 order=(here/'logs/player-build.log').read_text()
 assert order.index('physics2d_configuration_codegen:') < order.index('Physics2DDescription.cpp.obj')
 assert receipt['cold_build_correction']['status']=='PASS_MISSING_GENERATED_INPUTS'
 results={x['id']:x for x in receipt['test_results']}
 assert {f'X06-{n:02}' for n in range(1,7)}<=results.keys() and all(x['status']=='PASS' for x in results.values())
 for name in ['architecture','migration-ledger','test-coverage']:
  d=read(here/(name+'.json'));assert d['current_phase']=='P06' and d['implementation_sha']==impl and d['p06']['status']=='PASS'
 imports=(here/'logs/run-imports.log').read_text().lower()
 assert not any(x in imports for x in ['editor_app.dll','editor_context.dll','editor_ui.dll','editor_scene.dll','editor_storage.dll'])
 assert all(x['matched'] for x in read(here/'logs/portability/results.json'))
 assert all(x['exit_code']==0 for x in read(here/'logs/historical/results.json'))
 assert all(x['matched'] for x in read(here/'evidence/receipt-portability.json'))
 print('PASS P06 archive gate: actual runtime/runs, preserved regressions, PLAYER, SDK, dependency negatives, known FAILs retained')

if __name__=='__main__':
 parser=argparse.ArgumentParser();parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2]);args=parser.parse_args()
 check(args.repo.resolve())
