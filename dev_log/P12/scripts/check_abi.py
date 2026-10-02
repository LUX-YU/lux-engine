from pathlib import Path
import subprocess,json,hashlib,os
from datetime import datetime,timezone
w=Path(__file__).resolve().parent
workspace=Path(r'E:/SyncForder/CodeRepos/lux-engine-p12')
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=workspace,text=True).strip()
final=w/'final'/sha;out=final/'abi-independence';out.mkdir(parents=True,exist_ok=True)
records=[]
def run(name,args,cwd=workspace):
 args=list(map(str,args));log=out/(name+'.log');assert not log.exists()
 start=datetime.now(timezone.utc).isoformat()
 with log.open('wb') as f:result=subprocess.run(args,cwd=cwd,stdout=f,stderr=subprocess.STDOUT)
 records.append(dict(name=name,argv=args,cwd=str(cwd),started=start,ended=datetime.now(timezone.utc).isoformat(),exit_code=result.returncode,log=log.name,sha256=hashlib.sha256(log.read_bytes()).hexdigest(),implementation_sha=sha))
 (out/'commands.json').write_text(json.dumps(records,indent=2));print(name,result.returncode,flush=True)
 assert result.returncode==0,log.read_text(errors='replace')[-8000:]
probe=workspace.parent/'build'/('p12-abi-source-'+sha[:12]);build=workspace.parent/'build/RelWithDebInfo'/('p12-abi-'+sha[:12])
run('clone',['git','clone','--no-hardlinks','--no-checkout',workspace,probe]);run('checkout',['git','checkout','--detach',sha],probe)
base=next(x['argv'] for x in json.loads((final/'commands.json').read_text()) if x['name']=='configure').copy()
base[base.index('-S')+1]=str(probe);base[base.index('-B')+1]=str(build)
run('before-configure',base,probe)
header=probe/'editor/application/extensions/include/lux/engine/editor/extensions/EditorExtension.hpp'
version=header.read_text();assert 'kEditorExtensionVersion = 7;' in version
runtime_before=(build/'LuxPluginSdk.cmake').read_bytes()
abi=build/'gen/include/lux/engine/editor/extensions/EditorExtensionAbi.hpp'
assert abi.exists(),str(abi)
editor_before=abi.read_bytes();header.write_text(version.replace('kEditorExtensionVersion = 7;','kEditorExtensionVersion = 8;'))
(out/'isolated-editor-version.patch').write_bytes(subprocess.check_output(['git','diff','--',str(header)],cwd=probe))
run('after-editor-version-configure',base,probe)
runtime_after=(build/'LuxPluginSdk.cmake').read_bytes();editor_after=abi.read_bytes()
assert runtime_before==runtime_after and editor_before!=editor_after
run('verify-changed-source',['git','diff','--name-only'],probe)
(out/'result.json').write_text(json.dumps({'implementation_sha':sha,'runtime_identity_unchanged':True,'editor_identity_changed':True,'mutation':'isolated clone: EditorExtensionVersion 7 -> 8','runtime_before':runtime_before.decode(),'runtime_after':runtime_after.decode(),'editor_before':editor_before.decode(),'editor_after':editor_after.decode()},indent=2))
print('PASS production CMake identity remains independent of changed Editor contract',flush=True)
