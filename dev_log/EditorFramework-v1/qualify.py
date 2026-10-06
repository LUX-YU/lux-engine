from pathlib import Path
import json, subprocess, sys

source = Path('E:/SyncForder/CodeRepos/lux-engine')
record = source/'.internal/editor-redesign/framework-v1'
sha = subprocess.check_output(['git','rev-parse','HEAD'], cwd=source,text=True).strip()
clone = Path('E:/SyncForder/CodeRepos/qualification')/('framework-v1-'+sha[:8])
build = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo')/('framework-v1-'+sha[:8])
sdk = Path('E:/SyncForder/CodeRepos/install/EditorFramework-v1')
common = ['-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
 '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
 '-DCMAKE_PREFIX_PATH=E:/SyncForder/CodeRepos/install/EC3-recheck-5ec163ff8',
 '-DLuxLua55_DIR=E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/cmake/LuxLua55',
 '-DPython3_EXECUTABLE=C:/Users/ChenHui/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe',
 '-DLLVM_DIR=D:/Development/vcpkg/installed/x64-windows/share/llvm',
 '-DMLIR_DIR=D:/Development/vcpkg/installed/x64-windows/share/mlir',
 '-DBUILD_TESTING=ON','-DLUX_BUILD_EDITOR_LEGACY=OFF']
def run(name, *cmd, cwd=source, runtime=None):
    args=['py','-3',str(record/'run.py'),'--cwd',str(cwd),'--runtime',str(runtime or build/'bin'),sha[:8]+'-'+name,*map(str,cmd)]
    subprocess.run(args,check=True)

mode=sys.argv[1]
if mode=='editor':
    run('final-tracked','cmake',f'-DLUX_SOURCE_DIR={source}','-P','cmake/ValidateTrackedSnapshot.cmake')
    assert not clone.exists(), clone
    run('final-clone','git','clone','--no-hardlinks','--no-checkout',source,clone)
    run('final-checkout','git','checkout','--detach',sha,cwd=clone)
    (record/'qualification.json').write_text(json.dumps(dict(implementation_sha=sha,source=str(clone),build=str(build),sdk=str(sdk)),indent=2)+'\n')
    run('final-configure','cmake','-S',clone,'-B',build,*common,'-DLUX_BUILD_PROFILE=EDITOR',
        '-DLUX_BUILD_PACKED_RENDER_CONTENT=ON','-DLUX_EDITOR_BUILD_GPU_TESTS=ON','-DLUX_EDITOR_BUILD_DESKTOP_TESTS=ON',f'-DCMAKE_INSTALL_PREFIX={sdk}',cwd=clone)
    run('final-cold-build','cmake','--build',build,'--target','all','-j','4','--','-k','0',cwd=clone)
    run('final-second-build','cmake','--build',build,'--target','all','-j','4','--','-k','0',cwd=clone)
    run('final-ctest','ctest','--test-dir',build,'--output-on-failure',cwd=clone)
    assert not sdk.exists(), sdk
    run('final-install','cmake','--install',build,cwd=clone)
elif mode=='player':
    # Reconfigure the same clean source/build to PLAYER after Editor/SDK qualification.
    # Production targets are regenerated; no Editor targets may remain in the Ninja graph.
    player=build
    run('final-player-configure','cmake','-S',clone,'-B',player,*common,'-DLUX_BUILD_PROFILE=PLAYER',cwd=clone)
    run('final-player-build','cmake','--build',player,'--target','all','-j','4','--','-k','0',cwd=clone,runtime=player/'bin')
    run('final-player-second','cmake','--build',player,'--target','all','-j','4','--','-k','0',cwd=clone,runtime=player/'bin')
    run('final-player-ctest','ctest','--test-dir',player,'--output-on-failure',cwd=clone,runtime=player/'bin')
elif mode=='sdk':
    consumer=build.with_name(build.name+'-sdk')
    run('final-sdk-configure','cmake','-S',clone/'editor/tests/installed','-B',consumer,
        *common[:4],f'-DCMAKE_PREFIX_PATH={sdk};E:/SyncForder/CodeRepos/install/EC3-recheck-5ec163ff8',common[5],cwd=clone)
    run('final-sdk-build','cmake','--build',consumer,'--target','all','-j','4','--','-k','0',cwd=clone)
    run('final-sdk-ctest','ctest','--test-dir',consumer,'--output-on-failure',cwd=clone,runtime=sdk/'bin')
else:
    raise SystemExit('unknown qualification mode')
