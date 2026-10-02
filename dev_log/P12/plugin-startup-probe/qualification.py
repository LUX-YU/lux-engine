import os, subprocess, sys
prefix='E:/SyncForder/CodeRepos/install/P12-8d4aa55f840d'
os.environ['PATH']=prefix+'/bin;D:/Development/vcpkg/installed/x64-windows/bin;'+os.environ['SystemRoot']+'/System32'
sys.exit(subprocess.run(['D:/Development/Mircosoft/VisualStudio/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe','--test-dir','E:/SyncForder/CodeRepos/build/RelWithDebInfo/p12-installed-plugin-closure','--output-on-failure','-j','1']).returncode)
