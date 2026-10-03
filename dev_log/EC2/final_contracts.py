"""Exercise actual installed contracts and an intentionally stale owned SDK file."""
from pathlib import Path
import hashlib, json, subprocess, sys

w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text())
source=Path(c['source']);prefix=Path(c['prefix']);build=Path(c['build'])
compiler='D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/bin/clang-cl.exe'
args=[compiler,'/nologo','/std:c++20','/Zs','/EHsc','/MD','/utf-8','/permissive-',
      '/Zc:__cplusplus','/DWIN32','/D_WINDOWS','-imsvc'+str(prefix/'include'),
      '-imsvcD:/Development/vcpkg/installed/x64-windows/include',
      '-imsvcD:/Development/vcpkg/installed/x64-windows/include/eigen3',
      '-imsvcD:/Development/vcpkg/installed/x64-windows/include/stduuid']
def run(name,command,negative=False):
    result=subprocess.run([sys.executable,str(w/'run.py'),'--runtime',str(prefix/'bin'),
        '--cwd',str(source),name,*map(str,command)])
    assert (result.returncode!=0)==negative,name
    return (w/('logs/'+name+'.log')).read_text(errors='replace')
for kind in ['', 'MANIFEST','BYTES','OPEN']:
    name='final-publication-sdk-'+(kind.lower() or 'positive')
    text=run(name,[*args,*(['/DILLEGAL_'+kind] if kind else []),w/'after/publication-contract.cpp'],bool(kind))
    if kind:assert 'error:' in text and ('private' in text if kind=='BYTES' else 'const' in text)

# Only a nonexistent file under this qualification's freshly created SDK may be injected.
victim=prefix/'include/lux/engine/editor/storage/ProjectOpenData.hpp'
assert victim.resolve().is_relative_to(prefix.resolve()) and not victim.exists()
probe=build.with_name('ec2-final-stale-install-probe')
configure=['cmake','-S',source/'cmake/installed-consumers/editor-ec2','-B',probe,'-G','Ninja',
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_PREFIX_PATH='+prefix.as_posix(),
    '-DSDK_PREFIX='+prefix.as_posix(),'-DEC2_MODE=NATIVE',
    '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake']
run('final-retired-install-positive',configure)
injected=b'// Intentional qualification fixture: retired installed interface.\n'
try:
    victim.write_bytes(injected)
    text=run('final-retired-install-negative',configure,True)
    assert 'retired_installed_interface' in text
finally:
    assert victim.read_bytes()==injected
    victim.unlink()
run('final-retired-install-restored',configure)
run('final-retired-install-restored-build',['cmake','--build',probe,'--target','all','-j','4','--','-k','0'])
run('final-retired-install-restored-test',['ctest','--test-dir',probe,'--output-on-failure','-j','1'])
print('Actual installed immutable input contracts and stale-header positive/negative/restored qualification PASS')
