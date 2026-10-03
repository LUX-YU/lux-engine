from pathlib import Path
import argparse, subprocess, sys, json, shutil, hashlib
p=argparse.ArgumentParser();p.add_argument('phase',choices=['cold','postbuild']);a=p.parse_args()
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text())
source=Path(c['source']);build=Path(c['build']);prefix=Path(c['prefix']);sha=c['implementation_sha']
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip()==sha
assert not subprocess.check_output(['git','status','--porcelain'],cwd=source).strip()
def run(name,args,runtime=None):
 args=list(map(str,args));records=json.loads((w/'commands.json').read_text())
 old=next((x for x in records if x['name']==name),None)
 if old:
  assert old['argv']==args and old['exit_code']==0 and old['source_head']==sha,name
  assert hashlib.sha256((w/old['log']).read_bytes()).hexdigest()==old['sha256']
  print(name,'verified same-command evidence',flush=True);return
 subprocess.run([sys.executable,str(w/'run.py'),'--runtime',str(runtime or prefix/'bin'),
   '--cwd',str(source),name,*args],check=True)
common=['-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',f'-DCMAKE_PREFIX_PATH={prefix.as_posix()}',
 f'-DCMAKE_INSTALL_PREFIX={prefix.as_posix()}','-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
 '-DVCPKG_TARGET_TRIPLET=x64-windows','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
 '-DLuxLua55_DIR=E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/cmake/LuxLua55',
 '-DPython3_EXECUTABLE='+sys.executable,'-DLUX_EDITOR_MIGRATION_STAGE=EC2','-DLUX_EDITOR_LAYERING_MODE=STRICT',
 '-DLUX_BUILD_COMPONENT_LUA_META=OFF','-DBUILD_TESTING=ON','-DLUX_BUILD_PHYSICS2D=ON','-DLUX_BUILD_PACKED_RENDER_CONTENT=ON']
def all_build(name,path):run(name,['cmake','--build',path,'--target','all','-j','4','--','-k','0'])
if a.phase=='cold':
 run('final-tracked',['cmake','-DLUX_SOURCE_DIR='+str(source),'-P',source/'cmake/ValidateTrackedSnapshot.cmake'])
 query=build/'.cmake/api/v1/query';query.mkdir(parents=True,exist_ok=True)
 for name in ['codemodel-v2','cache-v2','toolchains-v1']:(query/name).touch()
 run('final-configure',['cmake','-S',source,'-B',build,*common,'-DLUX_BUILD_PROFILE=EDITOR',
  *['-DLUX_EDITOR_BUILD_'+group+'_TESTS=ON' for group in ['NATIVE','DESKTOP','GPU','TOOLCHAIN','INSTALLED']],
  '-DLLVM_DIR=D:/Development/vcpkg/installed/x64-windows/share/llvm',
  '-DMLIR_DIR=D:/Development/vcpkg/installed/x64-windows/share/mlir'])
 all_build('final-build',build)
elif a.phase=='postbuild':
 all_build('final-no-work',build)
 run('final-test-names',['ctest','--test-dir',build,'--show-only=json-v1'])
 run('final-ctest',['ctest','--test-dir',build,'--output-on-failure','-j','1','-R',r'^editor\.(compilation\..*|scene_views_gpu|presentation_.*|projection\.reset_capacity|architecture_current|projection_compilation_boundaries|desktop_view_boundaries|ec2\.boundaries)$'],build/'bin')
 shutil.copy2(build/'Testing/Temporary/LastTest.log',w/'final-ctest-details.log')
 run('final-install',['cmake','--install',build,'--prefix',prefix])
 shutil.copy2(build/'install_manifest.txt',w/'final-install-manifest.txt')
