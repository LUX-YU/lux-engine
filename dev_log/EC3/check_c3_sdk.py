from pathlib import Path
import subprocess,sys
root=Path(__file__).parent
runtime="E:/SyncForder/CodeRepos/install/EC3-C3/bin"
base="E:/SyncForder/CodeRepos/build/RelWithDebInfo/ec3-c3-staged-sdk-"
src="E:/SyncForder/CodeRepos/lux-engine-ec2/cmake/installed-consumers/"
def run(name,*cmd,expected=0):
 r=subprocess.run([sys.executable,str(root/"run.py"),"--runtime",runtime,name,*cmd])
 if r.returncode!=expected: raise SystemExit(r.returncode or 99)
def configure(name,source,extra=[]):
 run("c3-extracted-sdk-complete-"+name+"-configure","cmake","-S",(str(root/"sdk-test-input/models") if name=="models" else src+source),"-B",base+name,"-G","Ninja","-DCMAKE_BUILD_TYPE=RelWithDebInfo","-DCMAKE_PREFIX_PATH=E:/SyncForder/CodeRepos/install/EC3-C3","-Dlux-cxx_DIR=E:/SyncForder/CodeRepos/install/EC2-R1-ead7e59514a5/share/lux-cxx","-Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/install/EC2-R1-ead7e59514a5/share/lux-cmake-toolset","-DCMAKE_PROGRAM_PATH=E:/SyncForder/CodeRepos/install/EC2-R1-ead7e59514a5/bin","-DLUX_META_GENERATOR=E:/SyncForder/CodeRepos/install/EC2-R1-ead7e59514a5/bin/lux_meta_generator.exe","-DPython3_EXECUTABLE=C:/Users/ChenHui/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe","-Dimgui_DIR=E:/SyncForder/CodeRepos/install/EC2-R1-ead7e59514a5/share/imgui","-Dnode_editor_DIR=E:/SyncForder/CodeRepos/install/EC2-R1-ead7e59514a5/share/node_editor","-DLuxLua55_DIR=E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/cmake/LuxLua55","-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake",*extra)
for name,source in [("desktop","desktop-views")]:
 configure(name,source)
 run("c3-extracted-sdk-complete-"+name+"-build","cmake","--build",base+name,"--target","all","-j","4","--","-k","0")
 if name=="declarations": run("c3-extracted-sdk-complete-"+name+"-run",base+name+"/editor_ec3_declarations.exe")
 else: run("c3-extracted-sdk-complete-"+name+"-tests","ctest","--test-dir",base+name,"--output-on-failure","-E","native_input")
for n in range(4,10):
 name="negative-"+str(n)
 configure(name,"editor-ec3-declarations",["-DEC3_INVALID_DECLARATION="+str(n)])
 run("c3-extracted-sdk-complete-"+name+"-build","cmake","--build",base+name,"--target","all","-j","4","--","-k","0",expected=1)

