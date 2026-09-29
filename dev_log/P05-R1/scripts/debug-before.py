from pathlib import Path
import subprocess,os
repo=Path.cwd();out=repo/'.internal/editor-redesign/P05-R1-before'
os.environ['PATH']=str(repo.parent/'install/RelWithDebInfo/bin')+os.pathsep+'D:/Development/vcpkg/installed/x64-windows/bin'+os.pathsep+os.environ['PATH']
exe=repo.parent/'build/RelWithDebInfo/persistence-p05-consumer/persistence_consumer.exe'
for case in ['r1-revoke','r1-recursive']:
 with (out/(case+'-stack.log')).open('wb') as log:
  p=subprocess.run([r'C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe','-c','g; k; q',str(exe),str(out/'files'),case],stdout=log,stderr=subprocess.STDOUT,timeout=45)
 print(case,p.returncode,flush=True)
(out/'tests.patch').write_bytes(subprocess.check_output(['git','diff','--','editor/tests/persistence/models.cpp']))
