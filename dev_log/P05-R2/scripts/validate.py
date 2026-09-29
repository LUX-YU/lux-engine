"""Run exact-commit P05 qualification sequentially. All evidence stays in this work area until frozen."""
from pathlib import Path
import subprocess,json,hashlib,sys,os,shutil
workspace=Path(__file__).resolve().parents[2]
cluster=workspace.parent
work=workspace/'.internal/editor-redesign'
base=cluster/'build/RelWithDebInfo'
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=workspace,text=True).strip()
repo=cluster/'build/p05-r1-clean-f2b5a00c60e9'
build=base/'p05-r1-f2b5a00c60e9'
out=work/'P05-R2-run'/sha;out.mkdir(parents=True,exist_ok=True)
runtime_dirs=[]
record_file=out/'commands.json'
records=json.loads(record_file.read_text()) if record_file.exists() else []
def run(name,args,cwd=repo,expected=0):
 args=list(map(str,args));path=out/(name+'.log')
 existing=next((x for x in records if Path(x['log'])==path),None)
 if existing and existing['exit_code']==expected and existing['argv']==args and path.is_file():
  assert hashlib.sha256(path.read_bytes()).hexdigest()==existing['sha256']
  print(name,'verified prior same-commit run',flush=True);return
 if existing:
  archive=out/(name+'-failed-'+str(len(list(out.glob(name+'-failed-*'))))+'.log')
  if path.exists():archive.write_bytes(path.read_bytes())
  records.remove(existing)
 with path.open('wb') as log:
  p=subprocess.run(args,cwd=cwd,stdout=log,stderr=subprocess.STDOUT)
 records.append({'argv':args,'cwd':str(cwd),'exit_code':p.returncode,'log':str(path),
  'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'implementation_sha':sha,
  'archive_log':'dev_log/P05-R2/logs/'+path.name,'runtime_search_paths':runtime_dirs.copy()})
 record_file.write_text(json.dumps(records,indent=2)+'\n')
 print(name,p.returncode,flush=True)
 if p.returncode!=expected:
  print(path.read_text(errors='replace')[-12000:]);raise SystemExit(p.returncode or 1)
if not repo.exists():run('clone',['git','clone','--no-hardlinks','--no-checkout',workspace,repo],workspace)
run('clone-before-status',['git','status','--porcelain'],repo)
assert not (out/'clone-before-status.log').read_bytes()
run('clone-origin',['git','remote','get-url','origin'],repo)
run('clone-fetch',['git','fetch','origin',sha],repo)
run('clone-checkout',['git','checkout','--detach',sha],repo)
run('tracked-snapshot',['cmake',f'-DLUX_SOURCE_DIR={repo}','-P',repo/'cmake/ValidateTrackedSnapshot.cmake'])
cache={}
for line in (base/'lux-engine/CMakeCache.txt').read_text().splitlines():
 if '=' in line and not line.startswith(('//','#')):
  key,value=line.split('=',1);name,kind=key.split(':',1);cache[name]=(kind,value)
options=[]
for name,(kind,value) in cache.items():
 if name in ['CMAKE_BUILD_TYPE','CMAKE_PREFIX_PATH','CMAKE_TOOLCHAIN_FILE','VCPKG_TARGET_TRIPLET','Python3_EXECUTABLE','BUILD_TESTING'] or name.startswith('LUX_') and kind not in ('INTERNAL','STATIC'):
  options.append('-D'+name+'='+value)
 elif name.endswith('_DIR') and kind not in ('INTERNAL','STATIC') and value and Path(value).is_dir():
  directory=Path(value).resolve()
  if not directory.is_relative_to(workspace) and not directory.is_relative_to(base/'lux-engine'):
   options.append('-D'+name+'='+value)
run('configure',['cmake','-S',repo,'-B',build,'-G','Ninja',*options,'-DLUX_EDITOR_MIGRATION_STAGE=P05'])
for name in ['build','no-work']:run(name,['cmake','--build',build,'--target','all','-j','4','--','-k','0'])
run('ctest',['ctest','--test-dir',build,'--output-on-failure','-j','1'])
for name in ['content','atomic','identity','changes','plugin','mixed-1','mixed-2','mixed-3','mixed-4','read-5','read-6','read-7','read-8']:
 run('model-'+name,[build/'bin/editor_scene_model_test.exe',name],build/'bin')
for name in ['close-throw','close-contract']:
 run(name,[build/'bin/editor_sessions_test.exe',name],build/'bin')
run('sessions-detail',[build/'bin/editor_sessions_test.exe'],build/'bin')
for scenario in ['content','failure','layout','lifetime','mixed-replace','mixed-recreate','reading','reload-unbound','reload-unbound-lease','reload-closing-lease','reload-reading-lease','reload-outcomes']:
 run('material-'+scenario,[build/'bin/editor_material_model_test.exe',scenario],build/'bin')
for scenario in ['content','failure','mixed-signature','mixed-recreate','lifetime','reading',
 'reload-unbound','reload-unbound-external','reload-reading','reload-closing','reload-identity','reload-success',
 'input-stale','input-reading','input-closing','budgets','ids-undo-branch','ids-candidate-seed','ids-signature','ids-restore','ids-failure','ids-exhaustion']:
 run('flow-'+scenario,[build/'bin/editor_flowforge_model_test.exe',scenario],build/'bin')
run('three-sessions',[build/'bin/editor_three_sessions_test.exe'],build/'bin')
for scenario in ['models','order','close','close-key','capacity','execution','decoded','receipts','identities','conflict','measure','save-as','unknown','r1-revoke','r1-recursive','r1-chain','r1-chain-control','r1-callbacks','r1-reading','r1-conflicts','r2-accept-success','r2-accept-error','r2-accept-cancel','r2-accept-drain','r2-admission']:
 run('persistence-'+scenario,[build/'bin/editor_persistence_models_test.exe',out/'files',scenario],build/'bin')
run('persistence-files',[build/'bin/editor_publication_test.exe',out/'physical-files'],build/'bin')
run('persistence-coordinator',[build/'bin/editor_write_coordinator_test.exe'],build/'bin')
run('install',['cmake','--install',build,'--prefix',cluster/'install/RelWithDebInfo'])
groups=['editor-sessions-p01-consumer','ui-resources-editor-d2','ui-resources-external-feature','ui-scene-pane-consumer','ui-views-gpu-consumer']
for group in groups:
 cache=(base/group/'CMakeCache.txt').read_text();line=next(x for x in cache.splitlines() if x.startswith('CMAKE_HOME_DIRECTORY:'))
 source=Path(line.split('=',1)[1]);run(group+'-configure',['cmake','-S',source,'-B',base/group])
 for name in ['build','no-work']:run(group+'-'+name,['cmake','--build',base/group,'--target','all','-j','4','--','-k','0'])
group='scene-model-p02-consumer'
run(group+'-configure',['cmake','--fresh','-S',repo/'cmake/installed-consumers/scene-model','-B',base/group,'-G','Ninja',
 '-DCMAKE_BUILD_TYPE=RelWithDebInfo',f'-DCMAKE_PREFIX_PATH={cluster}/install/RelWithDebInfo',
 '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake','-DVCPKG_TARGET_TRIPLET=x64-windows'])
for name in ['build','no-work']:run(group+'-'+name,['cmake','--build',base/group,'--target','all','-j','4','--','-k','0'])
group='material-model-p03-consumer'
run(group+'-configure',['cmake','--fresh','-S',repo/'cmake/installed-consumers/material-model','-B',base/group,'-G','Ninja',
 '-DCMAKE_BUILD_TYPE=RelWithDebInfo',f'-DCMAKE_PREFIX_PATH={cluster}/install/RelWithDebInfo',
 '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake','-DVCPKG_TARGET_TRIPLET=x64-windows'])
for name in ['build','no-work']:run(group+'-'+name,['cmake','--build',base/group,'--target','all','-j','4','--','-k','0'])
group='flowforge-model-p04-consumer'
run(group+'-configure',['cmake','--fresh','-S',repo/'cmake/installed-consumers/flowforge-model','-B',base/group,'-G','Ninja',
 '-DCMAKE_BUILD_TYPE=RelWithDebInfo',f'-DCMAKE_PREFIX_PATH={cluster}/install/RelWithDebInfo',
 '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake','-DVCPKG_TARGET_TRIPLET=x64-windows'])
for name in ['build','no-work']:run(group+'-'+name,['cmake','--build',base/group,'--target','all','-j','4','--','-k','0'])
group='persistence-p05-consumer'
consumer_source=base/'persistence-p05-source';consumer_source.mkdir(parents=True,exist_ok=True)
shutil.copyfile(repo/'cmake/installed-consumers/persistence/CMakeLists.txt',consumer_source/'CMakeLists.txt')
shutil.copyfile(repo/'editor/tests/persistence/models.cpp',consumer_source/'main.cpp')
run(group+'-configure',['cmake','-S',consumer_source,'-B',base/group,'-G','Ninja',
 '-DCMAKE_BUILD_TYPE=RelWithDebInfo',f'-DCMAKE_PREFIX_PATH={cluster}/install/RelWithDebInfo',
 '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake','-DVCPKG_TARGET_TRIPLET=x64-windows'])
for name in ['build','no-work']:run(group+'-'+name,['cmake','--build',base/group,'--target','all','-j','4','--','-k','0'])
# SDK DLLs must be discoverable without a source/build-tree dependency.
runtime_dirs=[str(cluster/'install/RelWithDebInfo/bin'),'D:/Development/vcpkg/installed/x64-windows/bin']
os.environ['PATH']=os.pathsep.join(runtime_dirs)+os.pathsep+os.environ['PATH']
for group in groups+['scene-model-p02-consumer','material-model-p03-consumer','flowforge-model-p04-consumer','persistence-p05-consumer']:
 run(group+'-ctest',['ctest','--test-dir',base/group,'--timeout','120','--output-on-failure','-j','1'])
run('flow-installed-detail',[base/'flowforge-model-p04-consumer/flowforge_model_consumer.exe'],base/'flowforge-model-p04-consumer')
for probe in ['C01','C03','C04']:
 run(probe,[build/'bin/editor_baseline_failures.exe',probe,build,out/'probes'],build/'bin',expected=1)
run('boundaries',[sys.executable,repo/'editor/tests/architecture/test_editor_boundaries.py','--source',repo,'--cmake','cmake','--evidence',out/'boundaries.json'])
run('model-boundaries',[sys.executable,repo/'editor/tests/architecture/test_scene_model_boundaries.py','--source',repo,'--cmake','cmake','--evidence',out/'model-boundaries.json'])
run('material-boundaries',[sys.executable,repo/'editor/tests/architecture/test_scene_model_boundaries.py','--source',repo,'--cmake','cmake','--model','material','--evidence',out/'material-boundaries.json'])
run('flow-boundaries',[sys.executable,repo/'editor/tests/architecture/test_scene_model_boundaries.py','--source',repo,'--cmake','cmake','--model','flowforge','--evidence',out/'flow-boundaries.json'])
run('persistence-boundaries',[sys.executable,repo/'editor/tests/architecture/test_persistence_boundaries.py','--source',repo,'--cmake','cmake','--evidence',out/'persistence-boundaries.json'])
run('architecture',[sys.executable,repo/'editor/tests/architecture/check_editor_boundaries.py','--repo',repo,'--graph',build/'editor-architecture/targets.json','--compile-db',build/'compile_commands.json','--stage','P05'])
run('portable',[sys.executable,repo/'editor/tests/architecture/test_p01_evidence.py','--source',repo,'--evidence',out/'portability'])
run('historical-verifiers',[sys.executable,repo/'editor/tests/architecture/check_historical_evidence.py','--source',repo,'--evidence',out/'historical'])
run('original-p04-r1-gate',[sys.executable,repo/'dev_log/P04-R1/check_receipt.py'])
run('original-p04-gate',[sys.executable,repo/'dev_log/P04/check_receipt.py'])
run('original-p03-gate',[sys.executable,repo/'dev_log/P03/check_receipt.py'])
run('original-p03-r1-gate',[sys.executable,repo/'dev_log/P03-R1/check_receipt.py'])
run('package-audit',[sys.executable,workspace/'.internal/editor-redesign/spec-v4/scripts/audit_migration.py','--repo',repo,'--stage','P05','--output',out/'package-audit.json'])
run('test-names',['ctest','--test-dir',build,'--show-only=json-v1'])
run('model-imports',['dumpbin','/dependents',base/'scene-model-p02-consumer/scene_model_consumer.exe'])
run('material-imports',['dumpbin','/dependents',base/'material-model-p03-consumer/material_model_consumer.exe'])
run('flow-imports',['dumpbin','/dependents',base/'flowforge-model-p04-consumer/flowforge_model_consumer.exe'])
run('persistence-imports',['dumpbin','/dependents',base/'persistence-p05-consumer/persistence_consumer.exe'])
run('sessions-imports',['dumpbin','/dependents',base/'editor-sessions-p01-consumer/sessions_consumer.exe'])
run('original-p05-gate',[sys.executable,repo/'dev_log/P05/check_receipt.py'])
run('original-p05-r1-gate',[sys.executable,repo/'dev_log/P05-R1/check_receipt.py'])
run('clean-final',['git','status','--porcelain'],repo)
print('P05 R2 exact-SHA executable checks finished.',flush=True)
