from pathlib import Path
import subprocess, sys
w=Path(__file__).resolve().parent
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p12')
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=s,text=True).strip()
for phase in ['cold','developer','cpu','player','sdk','clang','regenerate']:
 assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=s,text=True).strip()==sha
 print('Sequential final phase:',phase,sha,flush=True)
 subprocess.run([sys.executable,w/'qualify.py','--phase',phase],check=True)
subprocess.run([sys.executable,w/'check_abi.py'],check=True)
print('Noninteractive matrix completed; native input and installed-menu qualification remain separate:',sha,flush=True)
