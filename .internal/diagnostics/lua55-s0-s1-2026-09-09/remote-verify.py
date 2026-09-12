"""Retrieve this stage's evidence from the remote into a fresh object database and verify every raw entry."""
import datetime, hashlib, json, os, re, subprocess, sys, zipfile
from pathlib import Path
root, evidence = (Path(p).resolve() for p in sys.argv[1:3])
commit = sys.argv[3]
assert re.fullmatch('[0-9a-f]{40}',commit)
source = Path(__file__).resolve().parents[3]
remote = subprocess.check_output(['git','-C',str(source),'remote','get-url','origin'],text=True).strip()
assert not Path(remote).exists(), 'Require a real remote, not the existing local object store'
relative = evidence.relative_to(source).as_posix()
out = root/'remote-evidence'
out.mkdir(exist_ok=False)
bare = out/'objects.git'
environment = dict(os.environ, GIT_TERMINAL_PROMPT='0')
commands = []
def run(args, target=None):
    command=['git',*args]
    with (out/'git.log').open('ab') as log:
        log.write(('COMMAND '+repr(command)+'\n').encode())
        if target is None:
            code=subprocess.call(command,stdout=log,stderr=subprocess.STDOUT,env=environment)
        else:
            with target.open('wb') as stream:
                code=subprocess.call(command,stdout=stream,stderr=log,env=environment)
    commands.append(dict(command=command,exit=code))
    (out/'commands.json').write_text(json.dumps(commands,indent=2))
    if code: raise RuntimeError(command)
run(['init','--bare',str(bare)])
run(['-C',str(bare),'remote','add','origin',remote])
run(['-C',str(bare),'config','remote.origin.promisor','true'])
run(['-C',str(bare),'config','remote.origin.partialclonefilter','blob:none'])
run(['-C',str(bare),'fetch','--depth=1','--filter=blob:none','origin',commit])
assert not (bare/'objects/info/alternates').exists()
retrieved=[]
for name in ['archives.json','raw-files.json','stage_result.json','SHA256SUMS']:
    run(['-C',str(bare),'show',f'{commit}:{relative}/{name}'],out/name)
    assert (out/name).read_bytes()==(evidence/name).read_bytes(), name
archives=json.loads((out/'archives.json').read_text())
raw=json.loads((out/'raw-files.json').read_text())
for archive in archives:
    name=archive['path']
    assert Path(name).name==name
    run(['-C',str(bare),'show',f'{commit}:{relative}/{name}'],out/name)
    digest=hashlib.sha256((out/name).read_bytes()).hexdigest()
    assert digest==archive['sha256']
    entries=[r for r in raw if r['archive']==name]
    with zipfile.ZipFile(out/name) as payload:
        assert len(payload.infolist())==len(entries)
        for entry in entries:
            assert hashlib.sha256(payload.read(entry['path'])).hexdigest()==entry['sha256']
    retrieved.append(dict(path=name,sha256=digest,verified_raw_entries=len(entries)))
record=dict(remote=remote,evidence_commit=commit,source_qualification='55dcb3fa85a9a2b7dcc87b178dd9ff76ba75c644',
    checked_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),fresh_object_database=True,
    local_alternates=False,archives=retrieved,verified_raw_entries=len(raw),
    raw_manifest_sha256=hashlib.sha256((out/'raw-files.json').read_bytes()).hexdigest(),commands=commands)
(out/'verification.json').write_text(json.dumps(record,indent=2))
print('REMOTE VERIFIED',commit,len(archives),'archives',len(raw),'raw entries')
