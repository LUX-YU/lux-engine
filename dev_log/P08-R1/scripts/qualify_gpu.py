"""Explicit installed scene-ui modes, after the main sequential qualification has finished."""
from pathlib import Path
import json,os,subprocess,hashlib,shutil
repo=Path(__file__).resolve().parents[2]
cluster=repo.parent;base=cluster/'build/RelWithDebInfo'
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
out=repo/'.internal/editor-redesign/P08-R1-run'/sha
records=json.loads((out/'commands.json').read_text())
assert any(Path(x['log']).stem=='clean-final' and x['exit_code']==0 for x in records)
source=cluster/'build/p05-r1-clean-f2b5a00c60e9'
paths=[str(cluster/'install/RelWithDebInfo/bin'),'D:/Development/vcpkg/installed/x64-windows/bin']
os.environ['PATH']=';'.join(paths)+';'+os.environ['PATH']
def run(name,args,cwd=source):
 args=list(map(str,args));path=out/(name+'.log')
 existing=next((x for x in records if x['log']==str(path)),None)
 if existing and existing['exit_code']==0:
  assert existing['argv']==args and hashlib.sha256(path.read_bytes()).hexdigest()==existing['sha256']
  return
 if existing:
  shutil.copyfile(path,out/(name+'-failed.log'));records.remove(existing)
 with path.open('wb') as log:
  code=subprocess.run(args,cwd=cwd,stdout=log,stderr=subprocess.STDOUT).returncode
 records.append(dict(argv=args,cwd=str(cwd),exit_code=code,log=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),implementation_sha=sha,archive_log='dev_log/P08-R1/logs/'+path.name,runtime_search_paths=paths))
 (out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
 print(name,code,flush=True)
 if code:
  print(path.read_text(errors='replace')[-8000:]);raise SystemExit(code)
for mode in ['GPU_UI','EDITOR_SCENE_PANE']:
 name='explicit-'+mode.lower();build=base/('p08-r1-'+mode.lower())
 run(name+'-configure',['cmake','-S',source/'cmake/installed-consumers/scene-ui','-B',build,'-G','Ninja',
  '-DCMAKE_BUILD_TYPE=RelWithDebInfo',f'-DCONSUMER_MODE={mode}',f'-DCMAKE_PREFIX_PATH={cluster}/install/RelWithDebInfo',
  '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake','-DVCPKG_TARGET_TRIPLET=x64-windows'])
 for action in ['build','no-work']:run(name+'-'+action,['cmake','--build',build,'--target','all','-j','4','--','-k','0'])
 run(name+'-ctest',['ctest','--test-dir',build,'-V','--timeout','120','-j','1'])
