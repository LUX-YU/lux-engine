from pathlib import Path
import argparse, subprocess, sys, json, shutil, hashlib
p=argparse.ArgumentParser();p.add_argument('phase',choices=['cold','postbuild','player','sdk','ec2-sdk','headers','skeleton']);a=p.parse_args()
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
 run('final-ctest',['ctest','--test-dir',build,'--output-on-failure','-j','1','-E','^editor.desktop_native_input$'],build/'bin')
 shutil.copy2(build/'Testing/Temporary/LastTest.log',w/'final-ctest-details.log')
 run('final-install',['cmake','--install',build,'--prefix',prefix])
 shutil.copy2(build/'install_manifest.txt',w/'final-install-manifest.txt')
elif a.phase=='player':
 player=build.with_name('ec2-final-player')
 run('final-player-configure',['cmake','-S',source,'-B',player,*common,'-DLUX_BUILD_PROFILE=PLAYER'])
 all_build('final-player-build',player);all_build('final-player-no-work',player)
 run('final-player-ctest',['ctest','--test-dir',player,'--output-on-failure','-j','1'],player/'bin')
 shutil.copy2(player/'Testing/Temporary/LastTest.log',w/'final-player-details.log')
 units=json.loads((player/'compile_commands.json').read_text())
 assert all('/editor/' not in (u['file']+' '+u['command']).replace('\\','/').lower() for u in units)
 print('PLAYER compile closure has no Editor source/include',len(units),flush=True)
elif a.phase=='headers':run('final-headers',[sys.executable,w/'headers.py'])
elif a.phase=='sdk':
 run('final-sdk',[sys.executable,source/'dev_log/P12/scripts/run_sdk.py','--source',source,
  '--prefix',prefix,'--output',w/'sdk','--build-root',build.with_name('ec2-final-sdk')])
elif a.phase=='ec2-sdk':
 run('final-ec2-sdk',[sys.executable,w/'sdk_ec2.py','--source',source,'--prefix',prefix,
  '--build',build.with_name('ec2-final-native-sdk'),'--label','final-ec2'])
elif a.phase=='skeleton':
 for mode in ['HEADLESS','WINDOW','APP']:
  path=build.with_name('ec2-final-skeleton-'+mode.lower())
  run('final-skeleton-'+mode.lower()+'-configure',['cmake','-S',source/'cmake/installed-consumers/editor-ec1-skeleton',
   '-B',path,*common,'-DEC1_MODE='+mode,'-DSDK_PREFIX='+prefix.as_posix(),
   '-DLLVM_DIR=D:/Development/vcpkg/installed/x64-windows/share/llvm',
   '-DMLIR_DIR=D:/Development/vcpkg/installed/x64-windows/share/mlir'])
  all_build('final-skeleton-'+mode.lower()+'-build',path);all_build('final-skeleton-'+mode.lower()+'-no-work',path)
  run('final-skeleton-'+mode.lower(),['ctest','--test-dir',path,'--output-on-failure','-j','1'])
  shutil.copy2(path/'Testing/Temporary/LastTest.log',w/('final-skeleton-'+mode.lower()+'-details.log'))
