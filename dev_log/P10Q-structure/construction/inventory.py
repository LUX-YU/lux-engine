from pathlib import Path
import hashlib, json, re, subprocess

original = Path(__file__).resolve().parents[3]
work = Path(__file__).resolve().parent
repo = original.parent / 'lux-engine-p10q-structure'
baseline_build = original.parent / 'build/RelWithDebInfo/p10q-3c20910d05d6'
rules = json.loads((work/'input/templates/path-rules.json').read_text())['rules']
manifest = json.loads((work/'input/SHA256.json').read_text())
for name, sha in manifest.items():
    assert hashlib.sha256((work/'input'/name).read_bytes()).hexdigest() == sha, name
files = subprocess.check_output(['git','ls-tree','-r','HEAD'], cwd=repo, text=True).splitlines()
blobs = {line.split('\t')[1]: line.split()[2] for line in files}
texts = {}
for name in blobs:
    if name.startswith(('dev_log/', '.internal/')): continue
    try: texts[name] = (repo/name).read_text(encoding='utf-8')
    except (UnicodeError, OSError): pass
reply = baseline_build/'.cmake/api/v1/reply'
index = json.loads(sorted(reply.glob('index-*.json'))[-1].read_text())
cm = json.loads((reply/index['reply']['codemodel-v2']['jsonFile']).read_text())
targets = {}
for entry in cm['configurations'][0]['targets']:
    target = json.loads((reply/entry['jsonFile']).read_text())
    targets[target['name']] = target
by_source = {}
for name, target in targets.items():
    for source in target.get('sources',[]): by_source.setdefault(source['path'],[]).append(name)
plan = []
for name, blob in blobs.items():
    if not name.startswith('editor/'): continue
    rule = max((r for r in rules if name == r['source'] or r['match']=='prefix' and name.startswith(r['source'])), key=lambda r:len(r['source']))
    dest = (rule['destination']+name[len(rule['source']):]) if rule['destination'] else name
    layer, batch, action, note = (rule[k] for k in ['layer','batch','action','note'])
    if name.startswith('editor/editing/scene/'):
        dest = name.replace('editor/editing/scene/','editor/activities/scene/')
        layer,batch,action,note='E2','L2','MOVE_EDIT','Actual RunStore pause editing provider; keep editor_editing_scene, one Registry algorithm.'
    elif name.startswith('editor/contracts/'):
        layer,batch='E0','L1'
        action='MERGE' if name.endswith(('CMakeLists.txt','README.md')) else 'MOVE'
        if name.endswith('ViewInfo.hpp'): action='SPLIT';layer='E0+E3'
    elif name.startswith('editor/editing/sinclude/') and not name.endswith('InteractionDelivery.hpp'):
        dest=name.replace('editor/editing/sinclude/','editor/activities/sinclude/')
        layer,batch,action='E2','L2','MOVE_EDIT'
    elif rule['id'] in ('F13','F14','F57','F58','F59','F60','F61','F62','F63','F64','F65'):
        layer,batch,action='RETAINED','L4','TEMPORARY_P12'
        note='Existing product consumer only; forbidden to new formal targets.'
        if rule['id'] in ('F58','F59'): action='TEMPORARY_P11'
        dest=name
        if name.endswith(('launcher/LaunchEditor.hpp','launcher/src/LaunchEditor.cpp')):
            dest=name.replace('editor/launcher/','editor/application/launch/');layer,batch,action='E4','L4','MOVE_EDIT'
    elif rule['id']=='F28':
        action='SPLIT' if name.endswith('CMakeLists.txt') else 'MOVE_EDIT'
    elif rule['id']=='F29':
        if name.endswith(('AssetImporter.hpp','AssetImporter.cpp')):
            layer,action='E2','MOVE_EDIT'
        elif name.endswith(('CMakeLists.txt','README.md')): layer,action='E2+RETAINED','SPLIT'
        else: dest=name;layer,action,batch='RETAINED','TEMPORARY_P12','L2'
    elif rule['id']=='F71':
        if '/workspace/recovery/' in name:
            dest=name.replace('editor/workspace/recovery/','editor/authoring/layout/');layer,batch,action='E1','L1','MOVE'
        elif name=='editor/workspace/README.md':dest='editor/authoring/layout/README.md';layer,batch,action='DOC','L1','MERGE'
        elif name=='editor/editing/sessions/README.md':dest='editor/editing/README.md';layer,batch,action='DOC','L1','MERGE'
        else: raise RuntimeError(name)
    if action=='REVIEW_REQUIRED': raise RuntimeError(name)
    if dest.endswith('CMakeLists.txt') and name!=dest: action='MERGE' if dest in [p['destination'] for p in plan] else action
    logical = name.split('/include/',1)[-1] if '/include/' in name else None
    consumers=[p for p,t in texts.items() if p!=name and (logical and logical in t or name in t)]
    owners=by_source.get(name,[])
    if not owners:
        parents=[(len(t['paths']['source']),n) for n,t in targets.items() if name.startswith(t['paths']['source'].rstrip('/')+'/') and t['type'] not in ('UTILITY',)]
        if parents:
            depth=max(x[0] for x in parents);owners=[n for d,n in parents if d==depth]
    plan.append(dict(source=name,blob=blob,destination=dest,layer=layer,batch=batch,action=action,
        note=note,logical_include=logical,targets=owners,consumers=consumers,verification='XL01-XL24; retained test names and assertions'))
assert len(plan)==518
(work/'file-plan.json').write_text(json.dumps(plan,ensure_ascii=False,indent=2),encoding='utf-8')
(work/'targets-before.json').write_text(json.dumps(targets,indent=2),encoding='utf-8')
names=subprocess.check_output(['ctest','--test-dir',str(baseline_build),'--show-only=json-v1'])
(work/'tests-before.json').write_bytes(names)
for cmd,filename in [(['status','--porcelain=v1','-z'],'worktree-before.bin'),(['diff','--binary'],'worktree-before.patch'),(['diff','--cached','--binary'],'index-before.patch'),(['log','-1','--format=fuller'],'commit-before.txt')]:
    (work/filename).write_bytes(subprocess.check_output(['git',*cmd],cwd=original))
state={'scope':'P10Q-structure','mode':'CONSTRUCTION','batch':'L0','status':'COMPLETE',
    'implementation_checkout':str(repo),'file_plan':'file-plan.json','targets':'targets-before.json','tests':'tests-before.json',
    'decisions':[
        {'id':'D01','decision':'ProjectBuilder is E1 pure config; user bytes excluded and patch relocated separately.'},
        {'id':'D02','decision':'ViewInfo remains E0; close errors move to actual view_api E3 provider.'},
        {'id':'D03','decision':'InteractionDelivery goes to workbench private; shared only by two graph tools.'},
        {'id':'D04','decision':'PersistenceAccess author gate/checkpoint remains E1; SaveSource is E2.'},
        {'id':'D05','decision':'Legacy product targets retained only for current named consumers until P11/P12.'},
        {'id':'D06','decision':'Keep existing binaries; real TaskMonitor and launch splits add STATIC targets.'},
        {'id':'D07','decision':'No FrozenEncoder template: domain encoding work differs; wrappers are not a shared algorithm.'},
        {'id':'D08','decision':'Extract single pure partition decoder into world_storage, remove scene_asset Process edge; async transport reuses decoder.'},
        {'id':'D09','decision':'RunStore actually uses SceneEditing; promote this one runtime Registry editing algorithm to E2, not a new bridge.'}],
    'scope_amendment':{'Linux':'NOT_RUN_NONBLOCKING','system_IME':'NOT_RUN','old_50k_samples':'DO_NOT_RESUME','C01':'original FAIL/P09-P12','C03':'original FAIL/P11','C04':'original FAIL/P12'},
    'next':'L1 pure partition decoder closure, then physical E0/E1 merge', 'verification':[]}
(work/'ledger.json').write_text(json.dumps(state,ensure_ascii=False,indent=2),encoding='utf-8')
ledger=original/'.internal/editor-redesign/migration-ledger.json'
data=json.loads(ledger.read_text(encoding='utf-8'));data['layering']={'ledger':'layering/ledger.json'}
ledger.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8')
print('L0:',len(plan),'files;',len(targets),'targets;',len(json.loads(names)['tests']),'tests; manifest',len(manifest))
