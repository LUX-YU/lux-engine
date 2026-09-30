from pathlib import Path
import os, subprocess, json, hashlib, shutil
repo=Path(__file__).resolve().parents[2]
out=repo/'.internal/editor-redesign/P08-R1-before'
out.mkdir(exist_ok=True)
build=repo.parent/'build/RelWithDebInfo/p08-r1-before-consumer'
prefix=repo.parent/'install/RelWithDebInfo'
os.environ['PATH']=str(prefix/'bin')+';D:/Development/vcpkg/installed/x64-windows/bin;'+os.environ['PATH']
records=[]
def run(name,args,expected):
 p=subprocess.run(list(map(str,args)),cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 path=out/(name+'.log');path.write_bytes(p.stdout)
 records.append(dict(name=name,argv=list(map(str,args)),exit_code=p.returncode,sha256=hashlib.sha256(p.stdout).hexdigest()))
 print(name,p.returncode,p.stdout.decode(errors='replace')[-1800:],flush=True)
 (out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
 assert p.returncode==expected
run('sdk-compile-link',['ninja','-t','commands','interaction_reclaim_consumer'],0)
run('sdk-build',['cmake','--build',build,'--target','interaction_reclaim_consumer','-j','4','--','-k','0'],0)
for model in ['scene','material','flow']:
 for mode in ['sync','cancel','selection']:
  run(model+'-'+mode,[build/'interaction_reclaim_consumer.exe',model,mode],1)
shutil.copyfile(repo/'editor/tests/persistence/interaction_reclaim.cpp',out/'interaction_reclaim.cpp')
shutil.copyfile(repo/'cmake/installed-consumers/interaction-views/CMakeLists.txt',out/'CMakeLists.txt')
artifacts=[]
for p in [*prefix.glob('lib/*interaction*.lib'),*prefix.glob('include/lux/engine/editor/*/*Interaction*.hpp')]:
 artifacts.append(dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
(out/'identity.json').write_text(json.dumps(dict(production_sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),sdk=artifacts,probe='Real installed public headers and libraries; no review-package shim; B slot last CodeLease callback; author encoding and SessionInfo checked'),indent=2)+'\n')
