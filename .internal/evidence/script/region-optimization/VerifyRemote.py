from pathlib import Path
import subprocess,hashlib,json,zipfile,io,datetime
root=Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-region-opt')
repo=root/'evidence-remote.git';assert not repo.exists()
origin='git@github.com:LUX-YU/lux-engine.git';commit='b52569f2818abf3fd146bcd830eb897767aa9c95'
out=root.parent/'s5/source/.internal/evidence/script/region-optimization'
log=(root/'remote-verification.log').open('w')
def run(cmd):
    log.write(json.dumps(cmd)+'\n');log.flush()
    p=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=log,check=True);return p.stdout
run(['git','init','--bare',str(repo)])
run(['git','-C',str(repo),'remote','add','origin',origin])
run(['git','-C',str(repo),'config','remote.origin.promisor','true'])
run(['git','-C',str(repo),'config','remote.origin.partialclonefilter','blob:none'])
run(['git','-C',str(repo),'fetch','--depth=1','--filter=blob:none','origin',commit])
assert run(['git','-C',str(repo),'rev-parse','FETCH_HEAD']).decode().strip()==commit
prefix='.internal/evidence/script/region-optimization/'
data=run(['git','-C',str(repo),'show',commit+':'+prefix+'region-optimization-raw-evidence.zip'])
index_bytes=run(['git','-C',str(repo),'show',commit+':'+prefix+'raw-files.json'])
summary=json.loads(run(['git','-C',str(repo),'show',commit+':'+prefix+'archive-summary.json']))
index=json.loads(index_bytes)
sha=hashlib.sha256(data).hexdigest();assert sha==summary['sha256']
assert sha==hashlib.sha256((out/summary['archive']).read_bytes()).hexdigest()
with zipfile.ZipFile(io.BytesIO(data)) as z:
    assert len(index)==len(z.namelist())==3273
    for row in index:
        item=z.read(row['path']);assert len(item)==row['size'] and hashlib.sha256(item).hexdigest()==row['sha256'],row['path']
record=dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),origin=origin,archive_commit=commit,
    method='Fresh bare partial repository fetched from GitHub; read archive/index from remote Git objects, no local object alternates',
    bare_repository=str(repo),archive=summary['archive'],sha256=sha,bytes=len(data),files=len(index),all_items_verified=True,
    index_sha256=hashlib.sha256(index_bytes).hexdigest(),local_archive_matches=True,
    qualification_source='402d5ca5ba0c2728bb4bdedda289f9eb36ce7e4e',
    branch_observed=run(['git','ls-remote',origin,'refs/heads/codex/s6-deep-optimization']).decode().strip())
(out/'remote-verification.json').write_text(json.dumps(record,indent=2));log.close();print(json.dumps(record),flush=True)
