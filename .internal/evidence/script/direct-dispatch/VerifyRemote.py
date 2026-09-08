"""Fetch evidence from GitHub in a fresh bare partial repository and verify every archived item."""
from pathlib import Path
import datetime,hashlib,io,json,subprocess,sys,zipfile
r=Path(__file__).resolve().parent;commit=sys.argv[1];repo=r/'evidence-remote.git';assert not repo.exists()
origin='git@github.com:LUX-YU/lux-engine.git';prefix='.internal/evidence/script/direct-dispatch/'
out=r.parent/'s5/source'/prefix;log=(r/'remote-verification.log').open('w')
def run(cmd):
 log.write(json.dumps(cmd)+'\n');log.flush();return subprocess.run(cmd,stdout=subprocess.PIPE,stderr=log,check=True).stdout
run(['git','init','--bare',str(repo)]);run(['git','-C',str(repo),'remote','add','origin',origin])
run(['git','-C',str(repo),'config','remote.origin.promisor','true']);run(['git','-C',str(repo),'config','remote.origin.partialclonefilter','blob:none'])
run(['git','-C',str(repo),'fetch','--depth=1','--filter=blob:none','origin',commit])
assert run(['git','-C',str(repo),'rev-parse','FETCH_HEAD']).decode().strip()==commit
blob=lambda name:run(['git','-C',str(repo),'show',commit+':'+prefix+name])
summary=json.loads(blob('archive-summary.json'));index_bytes=blob('raw-files.json');index=json.loads(index_bytes)
pieces=[]
for part in summary.get('parts',[dict(file=summary['archive'],bytes=summary['bytes'],sha256=summary['sha256'])]):
 data=blob(part['file']);assert len(data)==part['bytes'] and hashlib.sha256(data).hexdigest()==part['sha256'];pieces.append(data)
data=b''.join(pieces);assert len(data)==summary['bytes'] and hashlib.sha256(data).hexdigest()==summary['sha256']
with zipfile.ZipFile(io.BytesIO(data)) as z:
 assert len(z.namelist())==len(index)==summary['files']
 for row in index:
  item=z.read(row['path']);assert len(item)==row['size'] and hashlib.sha256(item).hexdigest()==row['sha256'],row['path']
record=dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),origin=origin,archive_commit=commit,archive=summary['archive'],sha256=summary['sha256'],bytes=len(data),files=len(index),all_items_verified=True,index_sha256=hashlib.sha256(index_bytes).hexdigest(),method='Fresh bare partial repository from GitHub; remote Git blobs, no local alternates',qualification_source=summary['qualification_source'],branch_observed=run(['git','ls-remote',origin,'refs/heads/codex/s6-deep-optimization']).decode().strip())
(out/'remote-verification.json').write_text(json.dumps(record,indent=2));log.close();(out/'remote-verification.log').write_bytes((r/'remote-verification.log').read_bytes());print(json.dumps(record),flush=True)
