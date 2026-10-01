"""Exercise the real frozen archive on relocated paths, with missing and tampered real evidence."""
from pathlib import Path
import subprocess,shutil,hashlib,json,sys
repo=Path(__file__).resolve().parents[2];archive=repo/'dev_log/P10Q'
receipt=json.loads((archive/'receipt.json').read_text());sha=receipt['implementation_sha']
replica=repo.parent/'build'/('p10q-archive-'+sha[:12]+'-迁移 proof')
assert not replica.exists(),'Each actual archive mutation probe uses a new directory'
shutil.copytree(archive,replica)
validator=repo/'editor/tests/architecture/validate_quality_evidence.py'
out=archive/'archive-probes';out.mkdir();records=[]
def run(name,location,expected):
    argv=[sys.executable,str(validator),'--source',str(repo),'--archive',str(location)]
    result=subprocess.run(argv,cwd=repo,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    path=out/(name+'.log');path.write_bytes(result.stdout)
    records.append({'name':name,'argv':argv,'expected_exit':expected,'exit_code':result.returncode,
        'log':path.name,'implementation_sha':sha,'sha256':hashlib.sha256(result.stdout).hexdigest()})
    assert result.returncode==expected,result.stdout.decode(errors='replace')
run('relocated-positive',replica,0)
target=replica/'logs/ctest.log';original=target.read_bytes()
target.unlink();run('missing-real-evidence',replica,1)
target.write_bytes(original+b'\nTAMPERED P10Q PROBE\n');run('tampered-real-evidence',replica,1)
target.write_bytes(original);run('restored-relocated-positive',replica,0)
(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
def manifest():
    items=[{'path':p.relative_to(archive).as_posix(),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
        for p in sorted(archive.rglob('*')) if p.is_file() and p.name not in ['receipt.json','artifacts.json']]
    (archive/'artifacts.json').write_text(json.dumps(items,indent=2)+'\n')
manifest()
run('canonical-final',archive,0)
(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
manifest()
result=subprocess.run([sys.executable,str(validator),'--source',str(repo),'--archive',str(archive)],cwd=repo)
assert result.returncode==0
print('Actual relocated archive accepted; missing/tampered real logs rejected; canonical archive verified')
