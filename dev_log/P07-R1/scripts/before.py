from pathlib import Path
import subprocess,os,json,hashlib
root=Path.cwd();work=root/'.internal/editor-redesign/P07-R1-before';base=root.parent/'build/RelWithDebInfo';records=[]
source=base/'p07-r1-before-source';build=base/'p07-r1-before-consumer'
os.environ['PATH']=str(root.parent/'install/RelWithDebInfo/bin')+os.pathsep+'D:/Development/vcpkg/installed/x64-windows/bin'+os.pathsep+os.environ['PATH']
options=[line.split('=',1)[0].split(':')[0]+'='+line.split('=',1)[1] for line in (base/'lux-engine/CMakeCache.txt').read_text().splitlines() if line.startswith(('MLIR_DIR:','LLVM_DIR:'))]
def run(name,args,expected):
 log=work/(name+'.log')
 with log.open('wb') as out: p=subprocess.run(list(map(str,args)),stdout=out,stderr=subprocess.STDOUT)
 records.append({'name':name,'argv':list(map(str,args)),'exit_code':p.returncode,'expected':expected,'sha256':hashlib.sha256(log.read_bytes()).hexdigest()})
 (work/'commands.json').write_text(json.dumps(records,indent=2))
 print(name,p.returncode,flush=True)
 if p.returncode!=expected: print(log.read_text(errors='replace')[-10000:]);raise SystemExit(1)
run('configure',['cmake','-S',source,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',f'-DCMAKE_PREFIX_PATH={root.parent}/install/RelWithDebInfo','-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake','-DVCPKG_TARGET_TRIPLET=x64-windows',*['-D'+x for x in options]],0)
run('build',['cmake','--build',build,'--target','all','-j','4','--','-k','0'],0)
run('traits',[build/'traits.exe'],0)
run('contract-negative',['cmake','--build',build,'--target','contract','-j','4','--','-k','0'],1)
for kind in ['material','flow']:
 for mode in ['control','copy']:run(kind+'-'+mode,[build/'before.exe',kind,mode],int(mode=='copy'))
(work/'compile_commands.json').write_bytes((build/'compile_commands.json').read_bytes())
print('BEFORE evidence complete',flush=True)
