from pathlib import Path
import subprocess, sys
w=Path(__file__).resolve().parent
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=s,text=True).strip()
for phase in ['cold','cpu','player','sdk','clang','regenerate']:
 assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=s,text=True).strip()==sha
 print('Sequential final phase:',phase,sha,flush=True)
 subprocess.run([sys.executable,w/'qualify.py','--phase',phase],check=True)
subprocess.run([sys.executable,w/'check_abi.py'],check=True)
print('Final matrix completed',sha,flush=True)
