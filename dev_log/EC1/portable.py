"""Exercise the actual EC1 verifier against relocated, missing and corrupted evidence."""
from pathlib import Path
import hashlib, json, shutil, subprocess, sys

w=Path(__file__).resolve().parent
source=Path('E:/SyncForder/CodeRepos/lux-engine-ec1')
archive=w/'archive'
root=w/'归档 可搬迁验证'
assert not root.exists(), 'Preserve prior probe evidence'
root.mkdir()
verifier=source/'editor/tests/architecture/validate_ec1_evidence.py'
records=[]
for name in ['relocated','missing','tampered']:
    target=root/name
    shutil.copytree(archive,target)
    log=root/(name+'.log')
    victim=target/'logs/final-build.log'
    if name=='missing':victim.unlink()
    if name=='tampered':
        with victim.open('ab') as stream:stream.write(b'\nintentional evidence corruption\n')
    argv=[sys.executable,str(verifier),'--source',str(source),'--archive',str(target)]
    with log.open('wb') as stream:
        result=subprocess.run(argv,stdout=stream,stderr=subprocess.STDOUT)
    expected=(result.returncode==0) if name=='relocated' else (result.returncode!=0)
    records.append(dict(name=name,argv=argv,exit_code=result.returncode,expected_result=expected,
                        log=log.name,sha256=hashlib.sha256(log.read_bytes()).hexdigest()))
    print(name,result.returncode,'expected',expected,flush=True)
    assert expected,name
(w/'portable-results.json').write_text(json.dumps(records,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('Actual archive relocation and missing/tamper rejection PASS; P12 historical waiver unchanged')
