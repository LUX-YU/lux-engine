from pathlib import Path
import json, shutil, subprocess, sys
w=Path(__file__).resolve().parent
c=json.loads((w/'final-config.json').read_text())
s=Path(c['source']);b=Path(c['build']);sdk=Path(c['prefix'])
records=json.loads((w/'commands.json').read_text())
check=next(x for x in records if x['name']=='resumed-ctest')
assert check['exit_code']==0 and check['source_head']==c['implementation_sha']
shutil.copy2(b/'Testing/Temporary/LastTest.log',w/'resumed-ctest-details.log')
for script, args in [('qualify.py',['install']),('qualify.py',['player']),('sdk.py',[])]:
    subprocess.run([sys.executable,str(w/script),*args],check=True)
subprocess.run([sys.executable,str(w/'run.py'),'--source',str(s),'--cwd',str(s),
    '--runtime',str(sdk/'bin'),'final-sdk-headers',sys.executable,str(w/'headers.py')],check=True)
subprocess.run([sys.executable,str(w/'run_controls_sdk.py')],check=True)
print('Remaining EC3 final qualification commands completed.',flush=True)
