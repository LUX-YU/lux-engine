from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, sys
p = argparse.ArgumentParser()
p.add_argument('phase', choices=['cold', 'tests', 'install', 'player'])
a = p.parse_args()
w = Path(__file__).resolve().parent
c = json.loads((w/'final-config.json').read_text())
s, b, sdk = [Path(c[k]) for k in ('source','build','prefix')]
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=s,text=True).strip() == c['implementation_sha']
assert not subprocess.check_output(['git','status','--porcelain'],cwd=s).strip()
def run(name, argv, runtime=None):
    argv = list(map(str,argv))
    old = next((r for r in json.loads((w/'commands.json').read_text()) if r['name']==name),None)
    if old:
        assert old['argv']==argv and old['exit_code']==0 and old['source_head']==c['implementation_sha'],name
        assert hashlib.sha256((w/old['log']).read_bytes()).hexdigest()==old['sha256']
        print(name,'verified existing exact-command evidence',flush=True)
        return
    subprocess.run([sys.executable,str(w/'run.py'),'--runtime',str(runtime or sdk/'bin'),
        '--cwd',str(s),name,*argv],check=True)
common = ['-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_PREFIX_PATH='+sdk.as_posix(),
    '-DCMAKE_INSTALL_PREFIX='+sdk.as_posix(), '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
    '-DVCPKG_TARGET_TRIPLET=x64-windows', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    '-DLuxLua55_DIR=E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/cmake/LuxLua55',
    '-DPython3_EXECUTABLE='+sys.executable, '-DLUX_META_GENERATOR_EXECUTABLE='+str(sdk/'bin/lux_meta_generator.exe'),
    '-DLUX_EDITOR_MIGRATION_STAGE=EC3','-DLUX_EDITOR_LAYERING_MODE=STRICT','-DLUX_BUILD_COMPONENT_LUA_META=OFF',
    '-DBUILD_TESTING=ON','-DLUX_BUILD_PHYSICS2D=ON','-DLUX_BUILD_PACKED_RENDER_CONTENT=ON']
def build(name, path): run(name,['cmake','--build',path,'--target','all','-j','4','--','-k','0'])
if a.phase=='cold':
    run('final-tracked',['cmake','-DLUX_SOURCE_DIR='+str(s),'-P',s/'cmake/ValidateTrackedSnapshot.cmake'])
    query=b/'.cmake/api/v1/query'
    query.mkdir(parents=True,exist_ok=True)
    for name in ('codemodel-v2','cache-v2','toolchains-v1'): (query/name).touch()
    run('final-configure',['cmake','-S',s,'-B',b,*common,'-DLUX_BUILD_PROFILE=EDITOR',
        *['-DLUX_EDITOR_BUILD_'+name+'_TESTS=ON' for name in ('NATIVE','DESKTOP','GPU','TOOLCHAIN','INSTALLED')],
        '-DLLVM_DIR=D:/Development/vcpkg/installed/x64-windows/share/llvm',
        '-DMLIR_DIR=D:/Development/vcpkg/installed/x64-windows/share/mlir'])
    build('final-build',b)
    build('final-no-work',b)
elif a.phase=='tests':
    run('final-test-list',['ctest','--test-dir',b,'--show-only=json-v1'])
    run('final-ctest',['ctest','--test-dir',b,'--output-on-failure','-j','1','-E','native_input'],b/'bin')
    shutil.copy2(b/'Testing/Temporary/LastTest.log',w/'final-ctest-details.log')
elif a.phase=='install':
    run('final-install',['cmake','--install',b,'--prefix',sdk])
    shutil.copy2(b/'install_manifest.txt',w/'final-install-manifest.txt')
elif a.phase=='player':
    player=b.with_name(b.name+'-player')
    run('final-player-configure',['cmake','-S',s,'-B',player,*common,'-DLUX_BUILD_PROFILE=PLAYER'])
    build('final-player-build',player)
    build('final-player-no-work',player)
    run('final-player-tests',['ctest','--test-dir',player,'--output-on-failure','-j','1'],player/'bin')
    shutil.copy2(player/'Testing/Temporary/LastTest.log',w/'final-player-details.log')
