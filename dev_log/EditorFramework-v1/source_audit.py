from pathlib import Path
import hashlib,json,re,subprocess
root=Path('E:/SyncForder/CodeRepos/lux-engine')
record=root/'.internal/editor-redesign/framework-v1'
def git(*args):return subprocess.check_output(['git',*args],cwd=root)
base='de315d48602c5155b919e9a698a9d9306f12decf'
sha=git('rev-parse','HEAD').decode().strip()
changes=git('diff','--name-status','--find-renames',base,sha).decode()
(record/'files.tsv').write_text(changes)
rows=[]
for old in git('ls-tree','-r','--name-only',base,'editor').decode().splitlines():
    if not old.endswith(('.cpp','.hpp')) or not any(x in old.split('/') for x in ['test','tests']):continue
    new='editor_legacy/'+old.removeprefix('editor/')
    before=git('show',base+':'+old).decode(errors='replace')
    after=(root/new).read_text(errors='replace')
    assertions=lambda s:re.findall(r'\b(?:assert|static_assert|CHECK|REQUIRE)\s*\(',s)
    a,b=len(assertions(before)),len(assertions(after))
    assert a==b,(old,a,b)
    rows.append(dict(original=old,current=new,assertion_count=a,byte_identical=before.replace('\r\n','\n')==after))
(record/'legacy-test-retention.json').write_text(json.dumps(rows,indent=2)+'\n')
print('Original test sources retained:',len(rows),'assertion sites:',sum(r['assertion_count'] for r in rows))
print('Changed test files (path/target spelling changes require review):')
for r in rows:
    if not r['byte_identical']:print(r['current'])
for p in (root/'editor').rglob('*'):
    if not p.is_file() or p.suffix not in ['.cpp','.hpp']:continue
    assert 'editor_legacy' not in p.read_text(),p
drive=[]
for p in (root/'editor').rglob('*.cpp'):
    if '/tests/' in p.as_posix():continue
    for i,line in enumerate(p.read_text().splitlines(),1):
        if 'driveFrame()' in line:drive.append((str(p.relative_to(root)),i))
assert len(drive)==1,drive
print('Unique production SceneRuntime drive:',drive)
print('Implementation SHA:',sha)
print('Source change manifest SHA256:',hashlib.sha256(changes.encode()).hexdigest())
