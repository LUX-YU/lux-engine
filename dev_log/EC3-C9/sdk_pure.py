from pathlib import Path
import hashlib,json,subprocess,sys
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text());s=Path(c['source']);p=Path(c['prefix']);base=s.parent/'build/RelWithDebInfo'/('ec3-sdk-'+c['implementation_sha'][:12])
common=['-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_PREFIX_PATH='+p.as_posix(),'-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
def run(name,args,expected=0):
 result=subprocess.run([sys.executable,str(w/'run.py'),'--source',str(s),'--cwd',str(s),'--runtime',str(p/'bin'),name,*map(str,args)])
 assert result.returncode==expected,name
b=base/'ec3-declarations-plain'
run('resumed-plain-declarations-configure',['cmake','-S',s/'cmake/installed-consumers/editor-ec3-declarations','-B',b,*common,'-DEC3_MEASURE_COMMANDS=OFF'])
for label in ['build','no-work']:run('resumed-plain-declarations-'+label,['cmake','--build',b,'--target','all','-j','4','--','-k','0'])
for exe in ['editor_ec3_declarations']:run('resumed-plain-'+exe,[b/(exe+'.exe')])
for n in range(1,10):
 b=base/('declaration-negative-'+str(n))
 label='resumed-declaration-negative-'+str(n)
 run(label+'-configure',['cmake','-S',s/'cmake/installed-consumers/editor-ec3-declarations','-B',b,*common,'-DEC3_INVALID_DECLARATION='+str(n)])
 run(label+'-build',['cmake','--build',b,'--target','all','-j','4','--','-k','0'],1)
 text=(w/'logs'/(label+'-build.log')).read_text(errors='replace')
 assert any(x in text for x in ['static_assert','constant expression','template argument','private member','C2672','C2131','C2248']),label
 assert 'C1083' not in text and 'LNK' not in text,label
print('Installed declarations and nine genuine compile rejections passed; desktop counters remain blocked.',flush=True)
