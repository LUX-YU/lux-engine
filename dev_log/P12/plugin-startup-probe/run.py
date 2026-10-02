import os, subprocess, sys
prefix='E:/SyncForder/CodeRepos/install/P12-8d4aa55f840d'
os.environ['PATH']=prefix+'/bin;D:/Development/vcpkg/installed/x64-windows/bin;'+os.environ['SystemRoot']+'/System32'
r=subprocess.run(['E:/SyncForder/CodeRepos/build/RelWithDebInfo/p12-plugin-startup-probe/plugin_startup_probe.exe',prefix])
sys.exit(r.returncode)
