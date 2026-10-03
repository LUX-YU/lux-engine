from pathlib import Path
import subprocess, json, hashlib, re

w = Path(__file__).resolve().parent
repo = Path('E:/SyncForder/CodeRepos/lux-engine-ec1')
original = Path('E:/SyncForder/CodeRepos/lux-engine')
base = '54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8'
def git(*args, cwd=repo):
    return subprocess.check_output(['git', *args], cwd=cwd)
sha = git('rev-parse', 'HEAD').decode().strip()
assert not git('status', '--porcelain').strip()
files = []
for path in git('diff', '--name-only', base, sha).decode().splitlines():
    proc = subprocess.run(['git', 'show', sha+':'+path], cwd=repo, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    files.append(dict(path=path, deleted=bool(proc.returncode), sha256=hashlib.sha256(proc.stdout).hexdigest() if not proc.returncode else None))
(w/'files.json').write_text(json.dumps(files,indent=2)+'\n')
assert not any(f['path'].startswith('dev_log/') for f in files)
patch = original/'editor/project/src/ProjectBuilder.cpp'
baseline = json.loads((w/'baseline.json').read_text())
protection = dict(original_head=git('rev-parse','HEAD',cwd=original).decode().strip(),
                  main=git('rev-parse','main',cwd=original).decode().strip(),
                  user_patch_applied=False, original_status=git('status','--short',cwd=original).decode(),
                  user_file_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
                  mapped_path='editor/authoring/project/src/ProjectBuilder.cpp',
                  preserved_archive='dev_log/P12/protected', historical_snapshot_changes=[])
assert protection['user_file_sha256']==baseline['protected_patch_sha256']
assert protection['original_head']==baseline['original_head'] and protection['main']==baseline['main']
assert 'editor/authoring/project/src/ProjectBuilder.cpp' not in {x['path'] for x in files}
(w/'protection.json').write_text(json.dumps(protection,indent=2)+'\n')
retired = re.compile(r'\b(?:PreparedSessionData|EProjectAssetKind|VCompiledSource|Bone_t)\b|EditHistory::(?:execute|undo|redo|clear|close)\b')
residual=[]; catches=[]; publics=[]
for path in git('ls-files','editor','engine','modules','cmake/installed-consumers').decode().splitlines():
    if Path(path).suffix not in ['.hpp','.h','.cpp','.cmake','.md']: continue
    text=(repo/path).read_text(encoding='utf-8-sig',errors='replace')
    for n,line in enumerate(text.splitlines(),1):
        if retired.search(line): residual.append(dict(path=path,line=n,text=line))
    if path in {x['path'] for x in files}:
        if '/include/' in path and Path(path).suffix in ['.hpp','.h']: publics.append(path)
        for n,line in enumerate(text.splitlines(),1):
            if re.search(r'\b(throw|catch|try)\b',line): catches.append(dict(path=path,line=n,text=line))
assert not residual, residual
host_skeleton=[]
for path in git('ls-files','editor/application').decode().splitlines():
    if Path(path).suffix not in ['.hpp','.cpp']:continue
    if 'skeleton' in (repo/path).read_text(errors='replace').lower():host_skeleton.append(path)
assert not host_skeleton
result=dict(implementation_sha=sha, files=len(files), public_headers=publics, retired_residual=residual,
            host_skeleton_branches=host_skeleton, exception_candidates=catches,
            semantic_review={
                'history':'EditHistoryData is the one phase/log owner; EditExecutor applies the original prepare/apply/publish/reclaim ordering. Model and Run consumers use it, not a second oracle.',
                'short_circuit':'DetachedView tests pane existence before attachedRoot/parent; source/model field indexes validate before lookup; callback and asynchronous identity rechecks retained.',
                'owning_input':'SessionPreparation, DetachedView and code-bearing artifacts keep external CodeLease until dynamic payloads/destructors complete, including rejected inputs. Existing R1 tests retained.',
                'exceptions':'No new domain throw or OOM recovery. Candidates in touched files are classified as foreign/factory/codec boundaries or existing tests; keyword scan is not an AST certificate.',
                'observers':'No new observer mutates Registry inline. Domain edits still patch through schema operations; readiness and GPU retirement remain polled at their original owners.',
                'headers':'Actual public definitions kept in their provider; Inspector contribution description separated from View. AssetTypeId moved to identity without changing its logical include.',
                'snapshot':'Only internal admitted immutable opaque blocks are retained; external freeze still copies. Full logical bytes continue to count against limits.',
                'checks':'No invalidation across the eliminated capture lookup: object is validated once with the same borrow; codec callback admission remains. Catalog revision checked before/after external codec work.',
                'limitations':'Bounded review of modified closure and actual callers, not a claim that all pre-existing repository style is repaired.'})
(w/'source-audit.json').write_text(json.dumps(result,indent=2)+'\n')
print('EC1 source deletion/owner/protection audit PASS',sha,'changed files',len(files),'public headers',len(publics))
