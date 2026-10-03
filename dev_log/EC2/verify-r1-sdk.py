from pathlib import Path
import subprocess, sys
w = Path(__file__).parent
prefix = Path('E:/SyncForder/CodeRepos/install/EC2-development')
compiler = 'D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/bin/clang-cl.exe'
args = [compiler, '/nologo', '/std:c++20', '/Zs', '/EHsc', '/MD', '/utf-8', '/permissive-',
        '/Zc:__cplusplus', '/DWIN32', '/D_WINDOWS', '-imsvc' + str(prefix / 'include'),
        '-imsvcD:/Development/vcpkg/installed/x64-windows/include',
        '-imsvcD:/Development/vcpkg/installed/x64-windows/include/eigen3',
        '-imsvcD:/Development/vcpkg/installed/x64-windows/include/stduuid']
for kind in ['', 'MANIFEST', 'BYTES', 'OPEN']:
    extra = ['/DILLEGAL_' + kind] if kind else []
    result = subprocess.run([sys.executable, str(w / 'run.py'), '--runtime', str(prefix / 'bin'),
        'r1-sdk-' + (kind.lower() or 'positive'), *args, *extra, str(w / 'after/publication-contract.cpp')])
    assert (result.returncode != 0) == bool(kind), kind
    if kind:
        text = (w / ('logs/r1-sdk-' + kind.lower() + '.log')).read_text()
        assert 'error:' in text and ('private' in text if kind == 'BYTES' else 'const' in text)
assert not (prefix / 'include/lux/engine/editor/storage/ProjectOpenData.hpp').exists()
print('Real installed SDK: positive compile, three immutability negatives, old header absent')
