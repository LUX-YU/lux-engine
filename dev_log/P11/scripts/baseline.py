from pathlib import Path
import hashlib, json, subprocess, shutil, re

w=Path(__file__).resolve().parent
r=Path('E:/SyncForder/CodeRepos/lux-engine-p11')
original=w.parents[2]
git=lambda *args:subprocess.check_output(['git',*args],cwd=r,text=True).strip()
sha=git('rev-parse','HEAD')
assert sha=='22ab1a30862f6bff943cef86d4232839173c7107'
assert not git('status','--porcelain')
assert git('rev-parse','origin/codex/editor-redesign-v4')==sha
checked=[]
for line in (w/'input/SHA256SUMS.txt').read_text().splitlines():
    digest,name=line.split(maxsplit=1)
    path=w/'input'/name.strip()
    assert path.resolve().is_relative_to((w/'input').resolve())
    assert hashlib.sha256(path.read_bytes()).hexdigest()==digest,name
    checked.append(name)
protected=original/'editor/project/src/ProjectBuilder.cpp'
assert hashlib.sha256(protected.read_bytes()).hexdigest()=='ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'
(w/'protected').mkdir(exist_ok=True)
for p in (original/'.internal/editor-redesign/layering/protected').iterdir():
    if p.is_file():shutil.copyfile(p,w/'protected'/p.name)
subprocess.run(['git','apply','--check',str(w/'protected/ProjectBuilder-relocated.patch')],cwd=r,check=True)
record={'input_sha':sha,'branch':git('branch','--show-current'),'implementation_checkout':str(r),
    'original_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=original,text=True).strip(),
    'main':subprocess.check_output(['git','rev-parse','main'],cwd=original,text=True).strip(),
    'user_patch_applied':False,'user_file_sha256':hashlib.sha256(protected.read_bytes()).hexdigest(),
    'archive_sha256':hashlib.sha256(Path('C:/Users/ChenHui/Downloads/LUX_ENGINE_P11_P12_CLOSEOUT_2026-10-01.zip').read_bytes()).hexdigest(),
    'archive_verified':checked,'previous_receipt':'dev_log/P10Q-structure/receipt.json'}
(w/'baseline.json').write_text(json.dumps(record,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
files=git('ls-files').splitlines()
content={p:(r/p).read_text(encoding='utf-8-sig',errors='replace') for p in files if p.endswith(('.cpp','.hpp','.h','.cmake','CMakeLists.txt','.py')) and not p.startswith('dev_log/')}
rules=json.loads((r/'editor/tests/architecture/rules.json').read_text())['editor_layering']
inventory=[]
for p in files:
    if not p.startswith(('editor/metadata/','editor/plugins/','editor/context/','editor/app/')):continue
    logical=p.split('/include/',1)[1] if '/include/' in p else None
    consumers=[f for f,s in content.items() if logical and re.search(r'#\s*include\s*[<"]'+re.escape(logical)+r'[>"]',s)]
    inventory.append({'path':p,'blob':git('rev-parse',sha+':'+p),'logical_include':logical,
        'providers':rules['files'].get(p,[]),'include_consumers':consumers,
        'action':'REVIEW_P11_CONTRIBUTION_OR_P12_PRODUCT','deadline':'P12','status':'P11-A inventory, per-symbol decision follows'})
(w/'source-map.json').write_text(json.dumps(inventory,indent=2)+'\n')
ledger_path=original/'.internal/editor-redesign/migration-ledger.json'
ledger=json.loads(ledger_path.read_text())
assert 'closeout' not in ledger
ledger['closeout']={'stage':'P11','batch':'A','status':'IN_PROGRESS','baseline':'P11/baseline.json',
    'source_map':'P11/source-map.json','checkout':str(r),'implementation_base':sha,
    'user_patch_applied':False,'stop_after':'P11','p12_authorized':False,
    'scope':{'Linux':'NOT_RUN','IME':'NOT_RUN','old_50k':'DO_NOT_RESUME'},
    'decisions':{'C03':'Preserve historical failure; fix current new and active old query lifetime in P11.',
                 'C04':'Original menu connection/create failure contract, not exit drain.',
                 'D04':'Split metadata per symbol: moved bodies removed now; named last old product consumers at P12.'}}
ledger_path.write_text(json.dumps(ledger,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print('P11 baseline verified:',len(checked),'package hashes;',len(inventory),'current legacy contribution/product files.')
