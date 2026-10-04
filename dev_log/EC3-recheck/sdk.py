from pathlib import Path
import json, shutil, subprocess, sys

w = Path(__file__).resolve().parents[1]
r = Path(__file__).resolve().parent
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec3-qualified')
p = Path('E:/SyncForder/CodeRepos/install/EC3-recheck-5ec163ff8')
b = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/ec3-recheck-sdk')
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=s, text=True).strip()
assert sha == 'aa4509995ea511c4a4306fc8477e44184ca39cf5'
assert not subprocess.check_output(['git', 'status', '--porcelain'], cwd=s).strip()

def run(name, args):
    subprocess.run([sys.executable, w/'run.py', '--source', s, '--cwd', s,
                    '--runtime', p/'bin', 'recheck-'+name, *map(str, args)], check=True)

common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_PREFIX_PATH='+p.as_posix(),
          '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
          '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DPython3_EXECUTABLE='+sys.executable,
          '-DLuxLua55_DIR=E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/cmake/LuxLua55']

for name, folder, extra in [
    ('settings', 'editor-ec3-settings', []),
    ('application', 'editor-application', ['-DSDK_PREFIX='+p.as_posix()]),
    ('skeleton', 'editor-ec1-skeleton', ['-DEC1_MODE=HEADLESS', '-DSDK_PREFIX='+p.as_posix()])
]:
    target = b/name
    run('sdk-'+name+'-configure', ['cmake', '-S', s/'cmake/installed-consumers'/folder, '-B', target, *common, *extra])
    for step in ['build', 'no-work']:
        run('sdk-'+name+'-'+step, ['cmake', '--build', target, '--target', 'all', '-j', '4', '--', '-k', '0'])
    run('sdk-'+name+'-tests', ['ctest', '--test-dir', target, '--output-on-failure', '-j', '1'])
    shutil.copy2(target/'Testing/Temporary/LastTest.log', r/('sdk-'+name+'-details.log'))
    if name == 'skeleton':
        run('sdk-skeleton-window', [target/'bin/skeleton_consumer.exe', target, p, 'WINDOW'])

run('sdk-desktop-gpu', [sys.executable, s/'dev_log/P12/scripts/run_sdk.py', '--source', s, '--prefix', p,
    '--output', r/'sdk', '--build-root', b/'regression', '--only',
    'p11,p11-runtime,desktop-views,gpu-ui,editor-scene-pane'])

# Parse only the changed public headers with the second compiler and the newly installed prefix.
for name in ['SettingsContent', 'SettingsView']:
    tu = r/(name+'.cpp')
    tu.write_text('#include <lux/engine/editor/project/'+name+'.hpp>\n')
    run('header-'+name, ['D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/bin/clang-cl.exe',
        '/nologo', '/std:c++20', '/Zs', '/EHsc', '/MD', '/utf-8', '/permissive-', '/DWIN32', '/D_WINDOWS',
        *['-imsvc'+str(path) for path in [p/'include', Path('D:/Development/vcpkg/installed/x64-windows/include'),
             Path('D:/Development/vcpkg/installed/x64-windows/include/eigen3'),
             Path('D:/Development/vcpkg/installed/x64-windows/include/stduuid')]], tu])

print('SDK settings, application, real plugin, window, desktop/GPU and public headers complete', flush=True)
