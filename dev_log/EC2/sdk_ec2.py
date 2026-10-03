from pathlib import Path
import argparse, json, subprocess, sys
p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True)
p.add_argument('--prefix',type=Path,required=True)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--label',required=True)
p.add_argument('--modes',default='NATIVE,LUA,SCENE,ACTIVITIES,PANELS')
a=p.parse_args();w=Path(__file__).resolve().parent
for mode in a.modes.split(','):
 b=a.build/mode.lower()
 project=a.source/'cmake/installed-consumers/editor-ec2'
 if mode in ('ACTIVITIES','PANELS'):project=project/'activities'
 common=['-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_PREFIX_PATH='+a.prefix.as_posix(),
  '-DSDK_PREFIX='+a.prefix.as_posix(),'-DEC2_MODE='+mode,
  '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
  '-DLuxLua55_DIR=E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/cmake/LuxLua55',
  '-DPython3_EXECUTABLE='+sys.executable]
 for name,args in [('configure',['cmake','-S',project,'-B',b,*common]),
                   ('build',['cmake','--build',b,'--target','all','-j','4','--','-k','0']),
                   ('no-work',['cmake','--build',b,'--target','all','-j','4','--','-k','0']),
                   ('test',['ctest','--test-dir',b,'--output-on-failure','-j','1'])]:
  subprocess.run([sys.executable,str(w/'run.py'),'--runtime',str(a.prefix/'bin'),'--cwd',str(a.source),
      a.label+'-'+mode.lower()+'-'+name,*map(str,args)],check=True)
 units=json.loads((b/'compile_commands.json').read_text())
 for unit in units:
  command=unit['command'].replace('\\','/').lower()
  assert '/pinclude/' not in command and '/sinclude/' not in command
