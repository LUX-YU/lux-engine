from pathlib import Path
import subprocess, sys

root = Path(__file__).parent
sdk = 'E:/SyncForder/CodeRepos/install/EC3-C4'
build = 'E:/SyncForder/CodeRepos/build/RelWithDebInfo/ec3-c4-sdk-skeleton'
source = 'E:/SyncForder/CodeRepos/lux-engine-ec2/cmake/installed-consumers/editor-ec1-skeleton'
dependencies = 'E:/SyncForder/CodeRepos/install/EC2-R1-ead7e59514a5'

def run(name, *args):
    subprocess.run([sys.executable, str(root / 'run.py'), '--runtime', sdk + '/bin', name, *args], check=True)

run('c4-sdk-install', 'cmake', '--install', 'E:/SyncForder/CodeRepos/build/RelWithDebInfo/ec2-dev', '--prefix', sdk)
run('c4-sdk-skeleton-configure', 'cmake', '-S', source, '-B', build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DSDK_PREFIX=' + sdk, '-DEC1_MODE=HEADLESS', '-DCMAKE_PREFIX_PATH=' + sdk,
    '-Dlux-cxx_DIR=' + dependencies + '/share/lux-cxx',
    '-Dlux-cmake-toolset_DIR=' + dependencies + '/share/lux-cmake-toolset',
    '-DCMAKE_PROGRAM_PATH=' + dependencies + '/bin', '-DLUX_META_GENERATOR=' + dependencies + '/bin/lux_meta_generator.exe',
    '-DPython3_EXECUTABLE=' + sys.executable, '-Dimgui_DIR=' + dependencies + '/share/imgui',
    '-Dnode_editor_DIR=' + dependencies + '/share/node_editor',
    '-DLuxLua55_DIR=E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/cmake/LuxLua55',
    '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake')
run('c4-sdk-skeleton-build', 'cmake', '--build', build, '--target', 'all', '-j', '4', '--', '-k', '0')
run('c4-sdk-skeleton-headless', 'ctest', '--test-dir', build, '--output-on-failure')
run('c4-sdk-skeleton-window', build + '/bin/skeleton_consumer.exe', build, sdk, 'WINDOW')
