"""Standalone C++20 parse of every changed installed public header with the second compiler."""
from pathlib import Path
import subprocess, hashlib, json, os
from datetime import datetime, timezone
repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p12');work=Path(__file__).resolve().parent;cluster=repo.parent
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
prefix=cluster/'install'/('P12-'+sha[:12]);out=work/'final'/sha/'public-headers'
out.mkdir(parents=True,exist_ok=True)
changed=subprocess.check_output(['git','diff','--name-only','--diff-filter=ACMR',
    '0a6644d8781dd3aeea1da549a87dfd6303a1c1dc',sha],cwd=repo,text=True).splitlines()
candidates=[path for path in changed if '/include/' in path and path.endswith('.hpp')]
fixtures=[path for path in candidates if path.startswith(('cmake/installed-consumers/','editor/tests/'))]
headers=[path for path in candidates if path not in fixtures]
manifest=cluster/'build/RelWithDebInfo'/('p12-'+sha[:12])/'install_manifest.txt'
installed_paths={Path(p).resolve() for p in manifest.read_text().splitlines()}
for path in headers:
 assert (prefix/'include'/path.split('/include/',1)[1]).resolve() in installed_paths,path
(out/'selection.json').write_text(json.dumps({'implementation_sha':sha,'public_headers':headers,'test_fixture_headers':fixtures,'policy':'Only fixture source trees are not SDK include providers; every production header must appear in actual install_manifest.'},indent=2))
includes=[prefix/'include',Path('D:/Development/vcpkg/installed/x64-windows/include'),
    Path('D:/Development/vcpkg/installed/x64-windows/include/eigen3'),
    Path('D:/Development/vcpkg/installed/x64-windows/include/stduuid')]
compiler=Path('D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/bin/clang-cl.exe')
records=[]
for i,path in enumerate(headers):
    public=path.split('/include/',1)[1];installed=prefix/'include'/public
    expected=subprocess.check_output(['git','show',sha+':'+path],cwd=repo)
    # Git and the installed checkout may use different line endings.
    assert installed.read_bytes().replace(b'\r\n',b'\n')==expected.replace(b'\r\n',b'\n'),path
    tu=out/f'header-{i:02}.cpp';tu.write_text('#include <'+public+'>\n')
    args=[str(compiler),'/nologo','/std:c++20','/Zs','/EHsc','/MD','/utf-8','/permissive-',
        '/Zc:__cplusplus','/DWIN32','/D_WINDOWS',*[('-imsvc'+str(v)) for v in includes],str(tu)]
    log=out/f'header-{i:02}.log';assert not log.exists(),'Do not replace existing parse evidence'
    started=datetime.now(timezone.utc).isoformat()
    with log.open('wb') as stream:result=subprocess.run(args,cwd=repo,stdout=stream,stderr=subprocess.STDOUT)
    ended=datetime.now(timezone.utc).isoformat()
    records.append({'name':f'header-{i:02}','public_header':public,'source':path,'argv':args,'cwd':str(repo),'started':started,'ended':ended,'log':log.name,
        'exit_code':result.returncode,'implementation_sha':sha,'sha256':hashlib.sha256(log.read_bytes()).hexdigest()})
    print(public,result.returncode,flush=True)
(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
(out/'scope.txt').write_text('Standalone parse only, using the fresh installed prefix and external dependency includes. '
    'No source/private/build include. Package dependency closure and linking are independently tested by the SDK consumer groups.\n')
assert records and all(item['exit_code']==0 for item in records)
