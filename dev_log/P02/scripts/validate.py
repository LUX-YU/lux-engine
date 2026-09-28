"""Run exact-commit P02 qualification sequentially. All evidence stays in this work area until frozen."""
from pathlib import Path
import subprocess,json,hashlib,sys,os
repo=next(p for p in Path(__file__).resolve().parents if (p/'CMakeLists.txt').is_file() and (p/'.git').exists())
work=repo/'.internal/editor-redesign'
base=repo.parent/'build/RelWithDebInfo';build=base/'lux-engine'
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
out=work/'P02-run'/sha;out.mkdir(parents=True,exist_ok=True)
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
  'archive_log':'dev_log/P02/logs/'+path.name,'runtime_search_paths':runtime_dirs.copy()})
 record_file.write_text(json.dumps(records,indent=2)+'\n')
 print(name,p.returncode,flush=True)
 if p.returncode!=expected:
  print(path.read_text(errors='replace')[-12000:]);raise SystemExit(p.returncode or 1)
run('tracked-snapshot',['cmake',f'-DLUX_SOURCE_DIR={repo}','-P',repo/'cmake/ValidateTrackedSnapshot.cmake'])
run('configure',['cmake','-S',repo,'-B',build,'-DLUX_EDITOR_MIGRATION_STAGE=P02'])
for name in ['build','no-work']:run(name,['cmake','--build',build,'--target','all','-j','4','--','-k','0'])
run('ctest',['ctest','--test-dir',build,'--output-on-failure','-j','1'])
for name in ['content','atomic','identity','changes','plugin']:
 run('model-'+name,[build/'bin/editor_scene_model_test.exe',name],build/'bin')
for name in ['close-throw','close-contract']:
 run(name,[build/'bin/editor_sessions_test.exe',name],build/'bin')
run('sessions-detail',[build/'bin/editor_sessions_test.exe'],build/'bin')
run('install',['cmake','--install',build])
groups=['editor-sessions-p01-consumer','ui-resources-editor-d2','ui-resources-external-feature','ui-scene-pane-consumer','ui-views-gpu-consumer']
for group in groups:
 cache=(base/group/'CMakeCache.txt').read_text();line=next(x for x in cache.splitlines() if x.startswith('CMAKE_HOME_DIRECTORY:'))
 source=Path(line.split('=',1)[1]);run(group+'-configure',['cmake','-S',source,'-B',base/group])
 for name in ['build','no-work']:run(group+'-'+name,['cmake','--build',base/group,'--target','all','-j','4','--','-k','0'])
group='scene-model-p02-consumer'
run(group+'-configure',['cmake','-S',repo/'cmake/installed-consumers/scene-model','-B',base/group,'-G','Ninja',
 '-DCMAKE_BUILD_TYPE=RelWithDebInfo',f'-DCMAKE_PREFIX_PATH={repo.parent}/install/RelWithDebInfo',
 '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake','-DVCPKG_TARGET_TRIPLET=x64-windows'])
for name in ['build','no-work']:run(group+'-'+name,['cmake','--build',base/group,'--target','all','-j','4','--','-k','0'])
# SDK DLLs must be discoverable without a source/build-tree dependency.
runtime_dirs=[str(repo.parent/'install/RelWithDebInfo/bin'),'D:/Development/vcpkg/installed/x64-windows/bin']
os.environ['PATH']=os.pathsep.join(runtime_dirs)+os.pathsep+os.environ['PATH']
for group in groups+['scene-model-p02-consumer']:
 run(group+'-ctest',['ctest','--test-dir',base/group,'--timeout','120','--output-on-failure','-j','1'])
for probe in ['C01','C03','C04']:
 run(probe,[build/'bin/editor_baseline_failures.exe',probe,build,out/'probes'],build/'bin',expected=1)
run('boundaries',[sys.executable,repo/'editor/tests/architecture/test_editor_boundaries.py','--source',repo,'--cmake','cmake','--evidence',out/'boundaries.json'])
run('model-boundaries',[sys.executable,repo/'editor/tests/architecture/test_scene_model_boundaries.py','--source',repo,'--cmake','cmake','--evidence',out/'model-boundaries.json'])
run('architecture',[sys.executable,repo/'editor/tests/architecture/check_editor_boundaries.py','--repo',repo,'--graph',build/'editor-architecture/targets.json','--compile-db',build/'compile_commands.json','--stage','P02'])
run('portable',[sys.executable,repo/'editor/tests/architecture/test_p01_evidence.py','--source',repo,'--evidence',out/'portability'])
run('historical-r1-verifier',[sys.executable,repo/'dev_log/P01-R1/check_receipt.py'])
run('package-audit',[sys.executable,repo/'.internal/editor-redesign/spec-v4/scripts/audit_migration.py','--repo',repo,'--stage','P02','--output',out/'package-audit.json'])
run('test-names',['ctest','--test-dir',build,'--show-only=json-v1'])
run('model-imports',['dumpbin','/dependents',base/'scene-model-p02-consumer/scene_model_consumer.exe'])
run('sessions-imports',['dumpbin','/dependents',base/'editor-sessions-p01-consumer/sessions_consumer.exe'])
clone=repo.parent/'build'/('p02-clean-'+sha[:12]);cb=clone.parent/(clone.name+'-build')
if not clone.exists():run('clone',['git','clone','--no-hardlinks','--no-checkout',repo,clone])
run('clone-checkout',['git','checkout','--detach',sha],clone)
run('clone-tracked',['cmake',f'-DLUX_SOURCE_DIR={clone}','-P',clone/'cmake/ValidateTrackedSnapshot.cmake'],clone)
cache={}
for line in (build/'CMakeCache.txt').read_text().splitlines():
 if '=' in line and not line.startswith(('//','#')):
  key,value=line.split('=',1);name,kind=key.split(':',1);cache[name]=(kind,value)
options=[]
for name,(kind,value) in cache.items():
 if name in ['CMAKE_BUILD_TYPE','CMAKE_PREFIX_PATH','CMAKE_TOOLCHAIN_FILE','VCPKG_TARGET_TRIPLET','Python3_EXECUTABLE','BUILD_TESTING'] or name.startswith('LUX_') and kind not in ('INTERNAL','STATIC'):
  options.append('-D'+name+'='+value)
 elif name.endswith('_DIR') and kind not in ('INTERNAL','STATIC') and value and Path(value).is_dir():
  directory=Path(value).resolve()
  if not directory.is_relative_to(repo) and not directory.is_relative_to(build):
   options.append('-D'+name+'='+value)
run('clone-configure',['cmake','-S',clone,'-B',cb,'-G','Ninja',*options],clone)
run('clone-architecture',[sys.executable,clone/'editor/tests/architecture/check_editor_boundaries.py','--repo',clone,'--graph',cb/'editor-architecture/targets.json','--compile-db',cb/'compile_commands.json','--stage','P02'],clone)
print('P02 executable checks finished; freeze ledger/coverage and run receipt gate next.',flush=True)
