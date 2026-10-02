from pathlib import Path
import hashlib, json, subprocess, shutil
w=Path(__file__).resolve().parent
s=Path('E:/SyncForder/CodeRepos/lux-engine-p12')
o=s.with_name('lux-engine')
base='0a6644d8781dd3aeea1da549a87dfd6303a1c1dc'
git=lambda root,*args:subprocess.check_output(['git',*args],cwd=root)
assert git(s,'rev-parse','HEAD').decode().strip()==base
assert not git(s,'status','--porcelain').strip()
inp=w/'input/LUX_ENGINE_P12_START_AFTER_P11_R1_2026-10-01'
verified=[]
for line in (inp/'SHA256SUMS.txt').read_text().splitlines():
 if not line.strip():continue
 digest,name=line.split(maxsplit=1);name=name.lstrip('*')
 p=(inp/name).resolve();assert p.is_relative_to(inp.resolve())
 assert hashlib.sha256(p.read_bytes()).hexdigest()==digest,(name,digest)
 verified.append(name)
protected=w/'protected';protected.mkdir(exist_ok=True)
user=o/'editor/project/src/ProjectBuilder.cpp'
digest=hashlib.sha256(user.read_bytes()).hexdigest()
assert digest=='ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'
shutil.copyfile(user,protected/'ProjectBuilder.user.cpp')
patch=git(o,'diff','--binary','--','editor/project/src/ProjectBuilder.cpp')
(protected/'original.patch').write_bytes(patch)
(protected/'relocated.patch').write_bytes(patch.replace(b'editor/project/src/ProjectBuilder.cpp',b'editor/authoring/project/src/ProjectBuilder.cpp'))
def save(name,data):
 (w/name).write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
save('baseline.json',dict(base=base,checkout=str(s),branch=git(s,'branch','--show-current').decode().strip(),
 original=str(o),original_head=git(o,'rev-parse','HEAD').decode().strip(),main=git(o,'rev-parse','main').decode().strip(),
 user_sha256=digest,user_patch_applied=False,package_verified=verified,
 package_sha256='5fc01f4a2496252479122a86c120899043d753622d74ecba23fff1ee62551006'))
prior=json.loads((w.parent/'P11/source-map.json').read_text(encoding='utf-8'))
paths=set(git(s,'ls-files').decode().splitlines())
rows=[]
for item in prior:
 if item.get('deadline')!='P12' or item.get('path') not in paths:continue
 row=dict(item)
 row.update(before_blob=git(s,'rev-parse',base+':'+row['path']).decode().strip(),status='P12 PLANNED')
 rows.append(row)
save('removal-plan.json',rows)
commands=json.loads((w.parent/'P11/command-map.json').read_text(encoding='utf-8'))
save('behavior-map.json',commands)
save('tracked-inputs.json',[dict(path=p,blob=git(s,'rev-parse',base+':'+p).decode().strip()) for p in sorted(paths) if p.startswith('editor/')])
shutil.copyfile(s/'dev_log/P11-R1/logs/test-names.log',w/'tests-before.json')
p=w.parent/'migration-ledger.json';ledger=json.loads(p.read_text(encoding='utf-8'))
ledger['closeout']['p12']=dict(status='IMPLEMENTING',batch='A',base=base,checkout=str(s),
 baseline='P12/baseline.json',removal_plan='P12/removal-plan.json',behavior_map='P12/behavior-map.json',
 user_patch_applied=False,stop_after='P12',p13_authorized=False,
 scope={'Linux':'NOT_RUN','IME':'NOT_RUN','sanitizer':'NOT_RUN','old_50k':'DO_NOT_RESUME'})
p.write_text(json.dumps(ledger,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('P12 baseline fixed; manifest',len(verified),'retained rows',len(rows),'tracked editor files',sum(p.startswith('editor/') for p in paths))
