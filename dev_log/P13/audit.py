"""P13 bounded source/build/reference review, not an AST or new platform qualification."""
from pathlib import Path
import hashlib, json, re, subprocess

w = Path(__file__).resolve().parent
s = Path('E:/SyncForder/CodeRepos/lux-engine-p12')
b = s.parent / 'build/RelWithDebInfo/p12-dev'
sdk = s.parent / 'install/P12-85d2ed7ec6f9'
base = '32500d90e5e27cfd22be1ef6b29a5e3010efcb3c'
git = lambda *args: subprocess.check_output(['git', *args], cwd=s).decode().strip()
sha256 = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
write = lambda name, data: (w / name).write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
files = git('ls-files').splitlines()
roots = ['app', 'context', 'metadata', 'ui', 'tools', 'project', 'storage', 'assets', 'launcher']
assert all(not (s / 'editor' / root).exists() for root in roots)
removal = json.loads((s / 'dev_log/P12/removal-plan.json').read_text())
retired = [r['path'] for r in removal]
assert all(not (s / path).exists() for path in retired)
graph = json.loads((b / 'editor-architecture/targets.json').read_text())
production = [t for t in graph if t['SOURCE_DIR'].startswith(str(s / 'editor').replace('\\', '/'))
              and t['TYPE'].endswith('LIBRARY') and '/tests/' not in t['SOURCE_DIR']]
test_leaks = []
for target in production:
    for source in target['SOURCES'].split(';'):
        if re.search(r'(^|/)(test|tests)/', source) and source.endswith(('.cpp', '.c')):
            test_leaks.append([target['name'], source])
assert not test_leaks, test_leaks
write('build-audit.json', {'source_head':git('rev-parse', 'HEAD'), 'stage':'P13', 'mode':'STRICT',
    'retired_roots':roots, 'retired_paths_absent':retired, 'editor_library_count':len(production),
    'test_implementation_in_production':test_leaks, 'graph_sha256':sha256(b/'editor-architecture/targets.json'),
    'formal_targets_preserved':[t for t in production if t['name'] in ['editor_assets','editor_editing_scene','editor_storage','editor_project','layout_model','editor_bootstrap']],
    'limitations':['Text/file inventory is not an AST proof.', 'Generated/private provider boundaries are checked by existing compiler-provider and dependency tests.']})

protected = ['editor/authoring/project/src/ProjectBuilder.cpp',
    'editor/activities/project/src/AssetImporter.cpp',
    'editor/activities/workspace/src/LegacyWorkspaceImporter.cpp']
# Exact tracked names, not assumed old physical paths.
protected = [p for p in files if p.endswith(('ProjectBuilder.cpp','AssetImporter.cpp','LegacyWorkspaceImporter.cpp','SceneEditing.cpp','SceneEdit.cpp','FieldEdit.cpp','ProjectManifest.cpp'))]
for p in protected:
    assert git('hash-object',p)==git('rev-parse',base+':'+p),p
write('preserved-algorithms.json', [{'path':p,'sha256':sha256(s/p),'base_blob':git('rev-parse',base+':'+p)} for p in protected])

text_files = [p for p in files if p.startswith('editor/') and p.endswith(('.cpp','.hpp','.h')) and '/test/' not in p and '/tests/' not in p]
old = re.compile(r'\b(EditorContext|PaneManager|SceneRunSlot|ProjectRenderAssets|LegacyPersistenceState|GuiDocumentProvider|DocumentView|SceneEditorTestAccess|MaterialEditorTestAccess|FlowForgeEditorTestAccess)\b')
hits=[]
for p in text_files:
    for line,text in enumerate((s/p).read_text(encoding='utf-8-sig').splitlines(),1):
        if old.search(text): hits.append({'path':p,'line':line,'text':text})
assert all('ax::NodeEditor::EditorContext' in r['text'] for r in hits),hits
write('reference-audit.json', {'inspected_production_files':len(text_files),'retained_homonyms':hits,
    'homonym_reason':'ax::NodeEditor::EditorContext is the third-party node canvas, not the deleted Lux EditorContext.',
    'historical_inventory':'editor/tests/architecture/inventory_editor.py intentionally names original V4 owners for Git history inspection; it is not product code.',
    'logical_includes':'lux/engine/editor/project and storage remain public include identities; physical old roots are absent.'})

mapping=json.loads((s/'dev_log/P12/behavior-map.json').read_text())
search = [p for p in text_files if p.startswith(('editor/application/','editor/workbench/project/'))]
for row in mapping:
    ids = re.findall(r'lux\.editor\.[a-z.-]+',row['formal'])
    locations=[]
    for p in search:
        for line,text in enumerate((s/p).read_text(encoding='utf-8-sig').splitlines(),1):
            if any('"'+id+'"' in text for id in ids): locations.append({'path':p,'line':line})
    row['current_source_locations']=locations
    row['inherited_record']='dev_log/P12/behavior-map.json at '+base
    row['new_runtime_result']=False
    if row['old_id']=='lux.product.about': row['clarification']='Version is a disabled informational menu label, not an executable action.'
    if row['old_id']=='lux.window.show/<id>': row['review']='EditorViews.cpp show/showTool and existing open handlers preserve ViewHost ownership.'
    if row['old_id']=='lux.product.open-asset': row['review']='EditorApplication.cpp openRequested connects the fixed AssetReference to open_intents_; ProjectView only emits intent.'
write('feature-review.json',mapping)

generator=s/'editor/workbench/scene/codegen'
installed=sdk/'share/lux-engine-editor-scene-ui/scene_ui/cmake_scripts/codegen'
generator_files=[]
for p in files:
    if p.startswith('editor/workbench/scene/codegen/'):
        a=s/p; relative=a.relative_to(generator); z=installed/relative
        assert z.is_file() and sha256(a)==sha256(z),p
        generator_files.append({'source':p,'installed':z.relative_to(sdk).as_posix(),'sha256':sha256(a)})
assert not list((sdk/'include').rglob('*TestAccess*'))
write('generation-support-audit.json',{'files':generator_files,'comparison':'Unmodified P12 installed generator, not a new regeneration run',
    'test_access_headers_installed':False,'application_test_access':'Existing conditional friend only; no test implementation is compiled in product targets. Actual test TU belongs to editor_application_test.'})

receipt=json.loads((s/'dev_log/P12/receipt.json').read_text())
assert receipt['status']=='PARTIAL_USER_WAIVER'
names=['build','no-work','cpu-ctest','player-ctest','p11-ctest','p11-runtime-ctest','p11-models-ctest',
       'scene-model-ctest','material-model-ctest','flowforge-model-ctest','persistence-ctest','scene-execution-ctest',
       'projection-compilation-ctest','interaction-views-ctest','workspace-ctest','desktop-views-ctest',
       'scene-ui-ctest','gpu-ui-ctest','editor-scene-pane-ctest','native-input','sdk-native-input',
       'clang-public-headers','regenerate','regenerate-no-work','installed-editor-smoke']
evidence=[]
for name in names:
    item=next(r for r in receipt['commands'] if r['name']==name)
    assert item['exit_code']==0 and sha256(s/'dev_log/P12'/item['log'])==item['sha256']
    evidence.append({'name':name,'implementation_sha':receipt['implementation_sha'],'record_commit':base,
                     'path':'dev_log/P12/'+item['log'],'sha256':item['sha256'],'status':'INHERITED_NOT_RERUN'})
write('inherited-evidence.json',{'p12_status':receipt['status'],'records':evidence,
    'waiver':'Manual menu action chain and final P12 archive qualification remain waived; no inference from wrapper exit 0.',
    'scope':'Unmodified providers only. P13 command ingress is separately tested; this does not create a new platform qualification.'})
print('Audit:',len(retired),'removed paths,',len(production),'libraries,',len(mapping),'feature rows,',len(generator_files),'generator/support files; inherited',len(evidence),'exact records')
