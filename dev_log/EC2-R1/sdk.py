from pathlib import Path
import json, subprocess, sys
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text());s=Path(c['source']);p=Path(c['prefix'])
def run(name,args):
 subprocess.run([sys.executable,str(w/'run.py'),'--runtime',str(p/'bin'),'--cwd',str(s),name,*map(str,args)],check=True)
run('final-sdk',[sys.executable,s/'dev_log/P12/scripts/run_sdk.py','--source',s,'--prefix',p,
 '--output',w/'sdk','--build-root',s.parent/'build/RelWithDebInfo/ec2-r1-sdk',
 '--only','projection-compilation,desktop-views,gpu-ui,editor-scene-pane'])
run('final-headers',[sys.executable,w/'headers.py'])
b=s.parent/'build/RelWithDebInfo/ec2-r1-public-recipe'
run('final-public-recipe-configure',['cmake','-S',w/'before','-B',b,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
 '-DCMAKE_PREFIX_PATH='+p.as_posix(),'-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake'])
run('final-public-recipe-build',['cmake','--build',b,'--target','all','-j','4','--','-k','0'])
