from pathlib import Path
import subprocess,os,json,hashlib
repo=Path.cwd(); out=repo/'.internal/editor-redesign/P05-R1-before';out.mkdir(exist_ok=True)
exe=repo.parent/'build/RelWithDebInfo/persistence-p05-consumer/persistence_consumer.exe'
os.environ['PATH']=str(repo.parent/'install/RelWithDebInfo/bin')+os.pathsep+'D:/Development/vcpkg/installed/x64-windows/bin'+os.pathsep+os.environ['PATH']
results=[]
for case in ['r1-revoke','r1-recursive','r1-chain-control','r1-chain']:
 p=subprocess.run([str(exe),str(out/'files'),case],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 log=out/(case+'.log');log.write_bytes(p.stdout)
 results.append(dict(case=case,exit_code=p.returncode,argv=[str(exe),str(out/'files'),case],log=log.name,sha256=hashlib.sha256(p.stdout).hexdigest()))
 print(case,p.returncode,p.stdout.decode(errors='replace'),flush=True)
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
(out/'identity.json').write_text(json.dumps({'production_sha':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'consumer_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'tests_sha256':hashlib.sha256((repo/'editor/tests/persistence/models.cpp').read_bytes()).hexdigest()},indent=2))
