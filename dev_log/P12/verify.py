"""P12 archive verifier: only relative archive evidence and immutable source objects."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess

def check(source, archive):
 archive=archive.resolve()
 load=lambda n:json.loads((archive/n).read_text(encoding='utf8'))
 receipt=load('receipt.json');sha=receipt['implementation_sha'];base=receipt['input_sha']
 assert base=='0a6644d8781dd3aeea1da549a87dfd6303a1c1dc'
 assert receipt['phase']==receipt['migration_stage']==receipt['stop_after']=='P12'
 assert receipt['layering_mode']=='STRICT' and receipt['status']=='PASS' and not receipt['continuation_authorized']
 git=lambda *a:subprocess.check_output(['git',*a],cwd=source)
 git('merge-base','--is-ancestor',base,sha);git('merge-base','--is-ancestor',sha,'HEAD')
 assert not git('diff','--name-only',base,sha,'--','dev_log').strip()
 protected='editor/authoring/project/src/ProjectBuilder.cpp'
 assert git('show',base+':'+protected)==git('show',sha+':'+protected)
 entries=load('artifacts.json');hashes={x['path']:x['sha256'] for x in entries};assert len(hashes)==len(entries)>100
 def data(name):
  path=(archive/name).resolve();assert path.is_relative_to(archive) and path.is_file(),name
  value=path.read_bytes();assert hashlib.sha256(value).hexdigest()==hashes[name],name
  return value
 for name in hashes:data(name)
 files=load('files.json');assert {x['path'] for x in files}==set(git('diff','--no-renames','--name-only',base,sha).decode().splitlines())
 for item in files:
  result=subprocess.run(['git','show',sha+':'+item['path']],cwd=source,capture_output=True)
  assert bool(result.returncode)==item['deleted'],item['path']
  if not item['deleted']:assert hashlib.sha256(result.stdout).hexdigest()==item['sha256'],item['path']
 paths=git('ls-tree','-r','--name-only',sha,'editor').decode().splitlines()
 assert {p.split('/')[1] for p in paths if len(p.split('/'))>2}=={'editing','authoring','activities','workbench','application','tests'}
 for item in load('removal-plan.json'):assert item['path'] not in paths,item['path']
 semantics=load('g-semantic-map.json')
 for item in semantics['files']:
  old=git('show',semantics['basis']+':'+item['path']).decode()
  assert git('rev-parse',semantics['basis']+':'+item['path']).decode().strip()==item['original_blob']
  if item['migration']=='RETIRED_PRIVATE_FIXTURE_NO_STANDALONE_ASSERTIONS':
   assert not item['assertions'] and not re.search(r'\bassert\s*\(',old)
   assert item['path'] not in paths
   continue
  assert item['semantic_observations'] and item['replacements']
  for replacement in item['replacements']:
   assert replacement['anchor'] in git('show',sha+':'+replacement['path']).decode(),replacement
 commands={x['name']:x for x in receipt['commands']};assert len(commands)==len(receipt['commands'])
 for c in commands.values():
  assert c['implementation_sha']==sha and c['started']<=c['ended']
  assert c['sha256']==hashes[c['log']];data(c['log'])
 def passed(name):
  c=commands[name];assert c['exit_code']==0,name;return c
 for name in ['tracked-snapshot','configure','build','no-work','ctest','native-input','sdk-native-input','install','test-names',
              'long-path-sdk-configure','long-path-sdk-build','long-path-sdk-run','product-help','launcher-smoke','installed-editor-smoke','installed-menu','developer-install','developer-editor-smoke','cpu-configure','cpu-build','cpu-no-work','cpu-ctest',
              'player-configure','player-build','player-no-work','player-ctest','clang-public-headers','clang-build','clang-ctest',
              'regenerate','regenerate-no-work','abi-before-configure','abi-after-editor-version-configure']:
  passed(name)
 for name in ['configure','cpu-configure','player-configure']:
  assert '-DLUX_EDITOR_MIGRATION_STAGE=P12' in commands[name]['argv']
  assert '-DLUX_EDITOR_LAYERING_MODE=STRICT' in commands[name]['argv']
 for name in ['build','cpu-build','player-build','regenerate']:
  argv=commands[name]['argv'];assert argv[argv.index('--target')+1]=='all' and argv[argv.index('-j')+1]=='4' and argv[-2:]==['-k','0']
 for name in ['no-work','cpu-no-work','player-no-work','regenerate-no-work']:
  assert b'ninja: no work to do' in data(commands[name]['log'])
 current=json.loads(data(passed('test-names')['log']));old=load('tests-before.json')
 removed={t['name'] for t in old['tests']}-{t['name'] for t in current['tests']}
 mapping=load('regression-name-map.json');assert set(mapping['removed'])==removed
 for name, item in mapping['removed'].items():assert item['replacement_tests'] and item['reason'],name
 details=data('logs/ctest-details.log');native=data('logs/native-input-details.log')
 def test(name, raw=details):
  block=re.search(rb'\d+/\d+ Testing: '+re.escape(name.encode())+rb'\r?\n.*?time elapsed:.*?\n',raw,re.S)
  assert block and b'Test Passed.' in block[0],name
  return block[0]
 app=test('editor.application')
 for text in ['C01: malformed product layout preserves','C04: actual EditorApplication::create rejects',
              'X12-09: accepted encode/compile','Recent projects: real legacy-format read','Initial scene: installed project description','X12-01: real factory failure preserves']:
  assert text.encode() in app,text
 for name in ['editor.commands','editor.contributions','editor.sessions.installation','editor.product.boundaries',
              'editor.scene_views_gpu','editor.layering.delivery_constraints']:
  test(name)
 assert b'left/right highlight isolation and retirement verified' in test('editor.scene_views_gpu')
 input_result=test('editor.desktop_native_input',native)
 assert b'OS mouse drag -> generated Inspector preview/commit/undo' in input_result and b'validation_errors=0' in input_result
 sdk_input=test('installed.desktop.native_input',data('logs/sdk-native-input-details.log'))
 assert b'OS mouse drag -> generated Inspector preview/commit/undo' in sdk_input and b'validation_errors=0' in sdk_input
 for group in receipt['consumer_groups']:
  for step in ['configure','build','no-work','ctest']:passed(group+'-'+step)
  assert b'Test Passed.' in data('evidence/sdk/'+group+'-ctest-details.log'),group
 assert len(receipt['consumer_groups'])==24
 builtin_plugins=data('evidence/sdk/p11-runtime-ctest-details.log')
 assert b'installed.p12.builtin_plugin_closure' in builtin_plugins
 assert b'PASS installed built-in plugin closure, without statically linking/preloading the plugins' in builtin_plugins
 for domain in ['scene','material','flowforge']:passed('persistence-'+domain+'-isolated')
 for group,text in [('gpu-ui','PASS installed RenderFeature: three GPU frames'),('editor-scene-pane','PASS Presentation/RenderFeature through SceneRuntime:')]:
  assert text.encode() in data('evidence/sdk/'+group+'-ctest-details.log')
 abi=json.loads(data('evidence/abi/result.json'));assert abi['implementation_sha']==sha
 assert abi['runtime_identity_unchanged'] and abi['editor_identity_changed']
 assert all(receipt['defects'][x]['current_result']=='PASS' for x in ['C01','C03','C04'])
 assert not receipt['worktrees']['user_patch_applied']
 assert all(receipt['platform_scope'][x]=='NOT_RUN' for x in ['linux','system_ime','sanitizer'])
 assert b'ordinary_present=1 extended_present=1' in data(passed('long-path-sdk-run')['log'])
 for name in ['installed-editor-smoke','installed-menu','developer-editor-smoke']:
  assert b'PASS sole installed lux_editor' in data(passed(name)['log'])
 manual=json.loads(data('evidence/installed-menu/observations.json'))
 assert manual['implementation_sha']==sha and manual['installed_product']
 for action in ['open','edit','save','run','close']:
  entries=[x for x in manual['actions'] if x['action']==action]
  assert entries and all(x['observed'] for x in entries),action
  for entry in entries:
   for evidence in entry['evidence']:data('evidence/installed-menu/'+evidence)
 cleanup=json.loads(data('evidence/installation-cleanup.json'))
 git('merge-base','--is-ancestor',cleanup['implementation_sha'],sha)
 assert cleanup['applied']
 cleanup_scan=json.loads(data('evidence/installation-final-scan.json'))
 assert cleanup_scan['implementation_sha']==sha and all(x['absent_after'] for x in cleanup_scan['files'])
 assert all(x['absent_after'] for x in cleanup['files'])
 installed=data('evidence/install_manifest.txt').decode().replace('\\','/')
 closure=json.loads(data('logs/runtime-closure-install.json'))
 assert closure['implementation_sha']==sha and not closure['manual_copies_to_final_prefix']
 assert {entry['file'] for entry in closure['files']}=={'box2d.dll','glfw3.dll','nfd.dll'}
 for entry in closure['files']:
  assert entry['listed_in_clean_install_manifest'] and entry['installed_path'].replace('\\','/') in installed
 for target in ['editor_app','editor_context','editor_ui','editor_metadata','editor_editing','editor_launcher','editor_scene','editor_material','editor_flowforge','editor_project_tools','editor_settings']:
  assert not re.search(r'/(?:lux_engine_)?'+target+r'\.(?:lib|dll|a|so)(?:\s|$)',installed),target
  assert '/'+target+'/' not in installed,target

 print('PASS P12 archive: fixed source, portable real evidence, formal product, deletion, semantic regression, SDK and UI/GPU')

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--archive',type=Path,required=True);a=p.parse_args();check(a.source,a.archive)
