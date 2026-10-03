from pathlib import Path
import hashlib, json, re, subprocess

w = Path(__file__).resolve().parent
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
original = Path('E:/SyncForder/CodeRepos/lux-engine')
def git(*args, root=s):
    return subprocess.check_output(['git', *args], cwd=root).decode().strip()
def save(name, value):
    (w/name).write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
verified = []
for directory in (w/'input').iterdir():
    manifest = directory/('MANIFEST.sha256.json' if 'EC2' in directory.name else 'MANIFEST.json')
    data = json.loads(manifest.read_text(encoding='utf-8'))
    files = data.get('files') or [dict(path=k, sha256=v) for k,v in data['files_sha256'].items()]
    for f in files:
        raw = (directory/f['path']).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == f['sha256'], f
        verified.append(dict(package=directory.name, **f))
save('input-integrity.json', verified)
assert git('rev-parse','HEAD') == '248adc4576943cab83976afd8d1d5f31b63b70a9'
assert not git('status','--porcelain')
patch = original/'editor/project/src/ProjectBuilder.cpp'
assert hashlib.sha256(patch.read_bytes()).hexdigest() == 'ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'
baseline = dict(head=git('rev-parse','HEAD'), branch=git('branch','--show-current'), checkout=str(s),
    parent=git('rev-parse','HEAD^'), original_head=git('rev-parse','HEAD',root=original),
    original_status=git('status','--porcelain',root=original), main=git('rev-parse','main',root=original),
    patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(), patch_applied=False,
    mapped_path='editor/authoring/project/src/ProjectBuilder.cpp')
save('baseline.json',baseline)
symbols = {
 'RD01':['ProjectPublication','preparePublication','adoptPublication','publishProjectFiles'],
 'RD02':['SceneConfigurationElement','SceneCreationConfiguration','SceneProviderOption','AuthoringFacts'],
 'RD03':['ArtifactPresentation','settleArtifacts','DerivedArtifact','receiveArtifact'],
 'RD04':['ResultsPane','WorkspacePane','VResultIntent','VWorkspaceIntent'],
 'RD05':['MaterialPreviewStore','MaterialCompileKey','CompiledMaterial','CompiledFlow'],
 'RD06':['LegacyMigration','LayoutCommitReceipt','prepareLegacyMigration','continueMigration','layoutResult'],
 'RD07':['AssetImporter','ModelImportRequest'], 'RD08':['ProjectOpenData','readProjectOpenData'],
 'GS01':['ReadAssetImage','loadAsset','VfsAssetReadEndpoint','IAssetProvider'],
 'GS02':['PreparedScriptApiCapability','ScriptApiCapabilityPublication','ScriptInstanceCreateContext'],
 'GS03':['ScriptAbilityInvocation','LuaBoundary','lux_script_abilities'],
 'GS04':['ScriptRuntimeHost','ScriptRuntimeSystem','DelayAbility','PhysicsQuery2D','DeferredScriptHost']}
tracked = git('ls-files').splitlines()
records=[]
for name in tracked:
    if not name.startswith(('editor/','engine/','modules/','cmake/','docs/')): continue
    if not (name.endswith(('.hpp','.h','.cpp','.c','.cmake','.py','.md')) or name.endswith('CMakeLists.txt')): continue
    raw=(s/name).read_bytes()
    text=raw.decode('utf-8-sig',errors='replace')
    matches={key:[dict(line=i,symbol=term) for i,line in enumerate(text.splitlines(),1)
                  for term in terms if re.search(r'\b'+re.escape(term)+r'\b',line)]
             for key,terms in symbols.items()}
    matches={k:v for k,v in matches.items() if v}
    if matches:
        records.append(dict(path=name,blob=git('rev-parse','HEAD:'+name),sha256=hashlib.sha256(raw).hexdigest(),
                            logical_include=name.split('/include/',1)[1] if '/include/' in name else None,matches=matches))
save('source-consumers.json',records)
reply=Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/ec1-137b8f441faa/.cmake/api/v1/reply')
model=json.loads(next(reply.glob('codemodel-v2*')).read_text())
targets=[]
for t in model['configurations'][0]['targets']:
    d=json.loads((reply/t['jsonFile']).read_text())
    targets.append(dict(id=t['id'],name=t['name'],type=d['type'],dependencies=d.get('dependencies',[]),
        sources=[x['path'] for x in d.get('sources',[])],compile_groups=d.get('compileGroups',[]),link=d.get('link')))
save('baseline-target-graph.json',targets)
for name in ['lux-cxx','lux-cmake-toolset','imgui','imgui-node-editor']:
    root=s.parent/name
    baseline[name]=dict(head=git('rev-parse','HEAD',root=root),status=git('status','--porcelain',root=root))
save('baseline.json',baseline)
ledger=original/'.internal/editor-redesign/migration-ledger.json'
data=json.loads(ledger.read_text(encoding='utf-8-sig'))
assert 'ec2' not in data
data['ec2']=dict(status='R0_COMPLETE',input_sha=baseline['head'],implementation_checkout=str(s),
    materials=str(w),input_integrity='EC2/input-integrity.json',baseline='EC2/baseline.json',
    file_actions='EC2/source-consumers.json',target_graph='EC2/baseline-target-graph.json',
    decisions='EC2/r0-decisions.md',batches={f'R{i}':'PENDING' for i in range(1,10)},
    inherited={'EC1':'248adc4576943cab83976afd8d1d5f31b63b70a9','P12':'PARTIAL_USER_WAIVER',
               'Linux':'NOT_RUN','system_ime':'NOT_RUN','old_50k':'PARTIAL_NO_MORE_SAMPLES'},
    user_patch_applied=False,stop_after='EC2')
ledger.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('R0',len(verified),'input files;',len(records),'actual affected files;',len(targets),'actual targets')
