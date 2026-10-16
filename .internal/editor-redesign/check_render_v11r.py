"""Narrow S1/S2-R protection gate, in addition to unchanged historical v11 gate."""
import argparse, hashlib, json, subprocess
from pathlib import Path
BASE = 'ae77e17fcfe5549615b9bc01ffa5822246ab3314'
ALLOW = {
'modules/function/render/runtime/include/lux/engine/render/runtime/RenderClient.hpp',
'modules/function/render/runtime/test/TypedBatch.cpp',
'modules/function/render/runtime/test/TypedClient.cpp',
'modules/platform/ipc/include/lux/engine/platform/ipc/IpcMessage.hpp',
'modules/platform/ipc/test/TypedFixture.hpp',
'modules/platform/ipc/test/TypedMessage.cpp',
'modules/platform/ipc/test/TypedPlugin.cpp',
'.internal/editor-redesign/RENDER_V11R_IMPLEMENTATION.md',
'.internal/editor-redesign/check_render_v11r.py',
'.internal/editor-redesign/RENDER_V11R_VERIFICATION.md',
'.internal/editor-redesign/RENDER_V11R_EVIDENCE.json',
}
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
def git(*args):return subprocess.check_output(['git','-C',str(a.source),*args]).decode().strip()
changed=set(git('diff','--name-only',BASE).splitlines())
changed.update(git('ls-files','--others','--exclude-standard').splitlines())
assert changed <= ALLOW, changed-ALLOW
def tree(rev):
 return {line.split('\t')[1]:line.split('\t')[0].split()[2] for line in git('ls-tree','-rz',rev).split('\0') if line}
base,current=tree(BASE),tree('HEAD')
manifest=json.loads((a.source/'docs/render-v2/LEGACY_SOURCE_MANIFEST.json').read_text())
frozen=tree(manifest['source_sha']);assert len(manifest['files'])==719
for item in manifest['files']:
 name=item['destination'];raw=(a.source/name).read_bytes()
 literal=hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()
 assert item['blob']==base[name]==current[name]==frozen[item['source']]
 assert item['blob'] in (literal,git('hash-object',name)),name
snapshot=json.loads(Path('E:/RenderV2Rebuild/V11/evidence/preflight.json').read_text(encoding='utf-8-sig'))
user=Path('E:/SyncForder/CodeRepos/lux-engine')
def usergit(root,*args):return subprocess.check_output(['git','-C',str(root),*args]).decode().strip()
assert usergit(user,'status','--porcelain')==snapshot['user_status']
assert usergit(user,'rev-parse','HEAD')==snapshot['user_head']
for name,digest in snapshot['user_sha256'].items():
 assert hashlib.sha256((user/name).read_bytes()).hexdigest()==digest,name
for name,state in snapshot['dependencies'].items():
 assert usergit(user.parent/name,'rev-parse','HEAD')==state['sha']
 assert usergit(user.parent/name,'status','--porcelain')==state['status']
subprocess.run(['git','-C',str(a.source),'diff','--check'],check=True)
a.output.write_text(json.dumps({'baseline':BASE,'head':git('rev-parse','HEAD'),'changed':sorted(changed),'legacy':719,'historical_evidence':'UNCHANGED','physical_ipc':'UNCHANGED','status':'PASS (protection only)'},indent=2))
