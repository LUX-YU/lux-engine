from pathlib import Path
import hashlib, json, re, subprocess
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text())
s=Path(c['source']);original=Path('E:/SyncForder/CodeRepos/lux-engine');base='248adc4576943cab83976afd8d1d5f31b63b70a9'
def git(*args,root=s):return subprocess.check_output(['git',*args],cwd=root)
sha=git('rev-parse','HEAD').decode().strip();assert sha==c['implementation_sha']
assert not git('status','--porcelain').strip()
files=[]
for name in git('diff','--name-only',base,sha).decode().splitlines():
 result=subprocess.run(['git','show',sha+':'+name],cwd=s,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
 files.append(dict(path=name,deleted=result.returncode!=0,sha256=None if result.returncode else hashlib.sha256(result.stdout).hexdigest()))
assert not any(f['path'].startswith('dev_log/') for f in files)
(w/'files.json').write_text(json.dumps(files,indent=2)+'\n')
baseline=json.loads((w/'baseline.json').read_text())
protection=dict(original_head=git('rev-parse','HEAD',root=original).decode().strip(),
 main=git('rev-parse','main',root=original).decode().strip(),original_status=git('status','--short',root=original).decode(),
 user_file_sha256=hashlib.sha256((original/'editor/project/src/ProjectBuilder.cpp').read_bytes()).hexdigest(),
 user_patch_applied=False,mapped_path='editor/authoring/project/src/ProjectBuilder.cpp',
 preserved_archive='dev_log/P12/protected',historical_snapshot_changes=[])
assert protection['original_head']==baseline['original_head'] and protection['main']==baseline['main']
assert protection['user_file_sha256']==baseline['patch_sha256']
assert protection['mapped_path'] not in {f['path'] for f in files}
(w/'protection.json').write_text(json.dumps(protection,indent=2)+'\n')
retired=re.compile(r'\b(ProjectOpenData|MaterialPreviewStore|MaterialCompileKey|AssetImporter|LayoutCommitReceipt|ResultsPane|WorkspacePane)\b|\blayoutResult\(')
residual=[];exception_candidates=[];public=[]
changed={x['path'] for x in files}
for name in git('ls-files','editor','engine','modules','cmake/installed-consumers').decode().splitlines():
 if Path(name).suffix not in ('.hpp','.h','.cpp','.cmake'):continue
 text=(s/name).read_text(encoding='utf-8-sig')
 for n,line in enumerate(text.splitlines(),1):
  if retired.search(line):residual.append(dict(path=name,line=n,text=line))
  if name in changed and re.search(r'\b(throw|catch|try)\b',line):exception_candidates.append(dict(path=name,line=n,text=line))
 if name in changed and '/include/' in name:public.append(name)
assert not residual,residual
sync=[]
for path in public:
 if not path.startswith('modules/'):continue
 logical=path.split('/include/',1)[1];raw=(s/path).read_bytes()
 for prefix in ['Debug/include','RelWithDebInfo/include','Android/lux-engine/include']:
  target=s.parent/'install'/prefix/logical
  assert target.read_bytes().replace(b'\r\n',b'\n')==raw.replace(b'\r\n',b'\n'),str(target)
  sync.append(dict(source=path,destination=str(target),sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
(w/'module-header-sync.json').write_text(json.dumps(sync,indent=2)+'\n')
prefix=Path(c['prefix']);bad=[];stale=[]
for p in (prefix/'include/lux/engine').rglob('*.hpp'):
 if retired.search(p.read_text(errors='replace')):bad.append(str(p.relative_to(prefix)))
for p in prefix.rglob('*.cmake'):
 if re.search(r'CodeRepos[/\\]install[/\\](?:EC1|EC2-development)',p.read_text(errors='replace')):stale.append(str(p.relative_to(prefix)))
assert not bad and not stale,(bad,stale)
for name in ['lux/engine/editor/storage/ProjectOpenData.hpp','lux/engine/editor/assets/AssetImporter.hpp',
             'lux/engine/editor/material/MaterialPreviewStore.hpp']:
 assert not (prefix/'include'/name).exists(),name
(w/'installed-audit.json').write_text(json.dumps(dict(implementation_sha=sha,prefix=str(prefix),
 retired_residual=bad,old_prefix_references=stale,sdk_identities=[dict(path=str(p.relative_to(prefix)),
 sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in prefix.rglob('*SdkIdentity*') if p.is_file()]),indent=2)+'\n')
(w/'source-audit.json').write_text(json.dumps(dict(implementation_sha=sha,files=len(files),public_headers=public,
 retired_residual=residual,exception_candidates=exception_candidates,
 semantic_review={
 'permissions':'Immutable publication plans carry values. PreparedProjectPublication alone retains the original Storage reservation; workers never release it.',
 'execution':'ArtifactPublicationOperation reuses original ProjectPublicationOperation and WriteCoordinator; no publisher or task runtime clone.',
 'configuration':'UI and native callers use SceneConfigurationPreparation on one source-preserving draft.',
 'preview':'Compilation input and destination adoption identities differ; two destinations use one immutable result with separate original retirement.',
 'script':'Actual ScriptSystem mints instance identity and revokes native scope before backend destruction. Worker data never carries Registry or VM.',
 'completion':'Fixed pending outcomes survive original ingress BACKPRESSURE; reentrant callbacks are followed by slot lookup. Native control block outlives plugin payload destruction.',
 'transport':'Opaque Lua values require exact registered semantics/layout, trivial payload and fixed maximum; no raw nontrivial resume.',
 'headers':'Native asset target and installed consumer are independently linked without Lua/Editor; optional generated Lua projection installs Delay support.',
 'exceptions':'No new OOM recovery or hot-path catch. Existing foreign, codec and fallible factory containment remains; keyword candidates are recorded, not called a full AST proof.',
 'observer':'No added EnTT observer writes world state. Script writes use original deferred command barrier.',
 'scope':'Review is of modified responsibility closure; no claim all pre-existing styles or external platforms are repaired.'}),indent=2)+'\n')
print('EC2 source/installed/protection/module audit PASS',sha,len(files),len(public),len(sync))
