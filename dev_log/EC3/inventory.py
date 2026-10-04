"""Refresh actual implementation mappings in the sole mutable EC3 ledger."""
from pathlib import Path
import hashlib, json, re, subprocess
w=Path(__file__).resolve().parent
p=w.parent/'migration-ledger.json'
j=json.loads(p.read_text()); e=j['ec3']
c=json.loads((w/'final-config.json').read_text()); s=Path(c['source']); sha=c['implementation_sha']
base=e['baseline']['head']
def git(*args): return subprocess.check_output(['git',*args],cwd=s,text=True).strip()
tracked=git('ls-files').splitlines()
production=[f for f in tracked if f.startswith(('editor/','engine/','modules/','cmake/','examples/'))]
texts={f:(s/f).read_text(encoding='utf-8-sig',errors='replace') for f in production
       if f.endswith(('.cpp','.hpp','.h','.cmake','.py','.template','.json')) or f.endswith('CMakeLists.txt')}
changes=[]
for line in git('diff','--name-status','--no-renames',base,sha).splitlines():
    action,path=line.split('\t',1)
    parent=Path(path).parent
    while parent!=Path('.') and not (s/parent/'CMakeLists.txt').is_file(): parent=parent.parent
    cmake=(parent/'CMakeLists.txt').as_posix()
    changes.append(dict(action=action,path=path,cmake=cmake,
        blob=None if action=='D' else git('rev-parse',sha+':'+path)))
e['final_files']=changes
e['final_source']=c
e['current_references']={symbol:[dict(file=f,lines=[i for i,line in enumerate(t.splitlines(),1) if symbol in line])
    for f,t in texts.items() if symbol in t] for symbol in (
    'CommandDescriptor','CommandEntry::bind','SettingsEntry','SettingsPage','ViewportCameraState',
    'ProjectContentSaving','ProjectPluginSelection','RecentProjects','WorkspaceChanges','WorkspaceActions',
    'RestoreWorkbench','ProjectContentOpening','InspectorModel','lux_engine_inspector_generator')}
change_paths={x['path'] for x in changes}
for item in e['file_actions']:
    path=item['path']
    if path=='.internal/editor-redesign/':
        item['status']='SOLE_MUTABLE_LEDGER'; item['outcome']='This ec3 node; no parallel ledger'
    elif path=='dev_log/EC3/':
        item['status']='PENDING_FINAL_EVIDENCE'
    elif path=='dev_log/':
        item['status']='PRESERVED'; item['outcome']='No tracked historical changes in implementation diff'
        assert not any(f.startswith('dev_log/') for f in change_paths)
    else:
        matches=[f for f in tracked if f.startswith(path)] if path.endswith('/') else ([path] if path in tracked else [])
        item['current_files']=[dict(path=f,blob=git('rev-parse',sha+':'+f),changed=f in change_paths) for f in matches]
        item['status']='REMOVED' if not matches else ('CHANGED' if any(f in change_paths for f in matches) else 'RETAINED_UNIQUE_PROVIDER')
        item['qualification']='Source disposition only; behavior maps to final coverage, not automatic PASS'

destinations={
 'prepareSave':'ProjectContentSaving::prepare', 'rememberSave':'ProjectContentSaving::track',
 'settleSaves':'ProjectContentSaving::update', 'maintainRecentProjects':'RecentProjects::update',
 'installRecentProjects':'RecentProjectsView factory/commands plus application launch receiver',
 'executeWorkspaceIntent':'WorkspaceActions / WorkspaceChanges; application routes typed intents',
 'settleWorkspace':'WorkspaceChanges::update and original ViewHost safe-point composition',
 'observeWorkspace':'WorkspaceChanges query; WorkspaceView presentation',
 'applyLayout':'WorkspaceActions::apply; application checks product admission',
 'restoreRecovery':'RestoreWorkbench::restore', 'settleRecovery':'RestoreWorkbench::update',
 'captureRecovery':'RestoreWorkbench::capture', 'settleMigration':'WorkspaceChanges migration',
 'openCaptured':'ProjectContentOpening::open', 'openStatus':'ProjectContentOpening::status',
 'cancelOpen':'ProjectContentOpening::cancel', 'acknowledgeOpen':'ProjectContentOpening::acknowledge',
 'maintainProjectSettings':'ProjectPluginSelection publication plus application typed intent/status routing',
 'showSceneTool':'makeSceneToolCommand: typed ESceneTool; application explicit service assembly',
 'installSceneCommands':'SceneCommands module descriptors; RunStore and tool service bindings',
 'installContentCommands':'SessionCommands module; no central command descriptor copy',
 'installSaveCommands':'Session/Project command declarations plus product review receiver',
 'makeContentView':'module ViewFactory, explicit dependency binding',
 'showTool':'ViewHost showToolWindow; dynamic tool command generated at registration',
 'execute':'original Dispatcher; no SaveOperation interpretation',
 'installContributions':'module-owned fixed contributions; original compound guard',
}
for item in e['application_methods']:
    name=item['method']; pattern=re.compile(r'EditorApplication(?:::Impl)?::'+re.escape(name)+r'\s*\(')
    found=[dict(file=f,lines=[t.count('\n',0,m.start())+1 for m in pattern.finditer(t)])
           for f,t in texts.items() if f.startswith('editor/application/src/') and pattern.search(t)]
    item['current_definitions']=found
    item['final_owner']=destinations.get(name,'Application product assembly/admission/dialog or lifecycle; original domain owner retained')
    item['disposition']='REMOVED_OR_MIGRATED' if not found else 'RETAINED_COMPOSITION_OR_ADMISSION'
e['camera_audit']=[
 dict(symbol='cameraMotion/cameraRay',owner='viewport pure math',frequency='input only',decision='RETAIN distinct navigation and projection math; no resource ownership'),
 dict(symbol='SceneView::imageRay',owner='Scene workbench',frequency='pick/drop input',decision='RETAIN source/camera/output readiness; normalize logical input once against sampled extent'),
 dict(symbol='ViewportPresentation::setCameraPose',owner='viewport resource owner',frequency='navigation/state adoption',decision='COMPARE current exact values; patch only changed original ECS components and one request revision'),
 dict(symbol='ViewportPresentation private constructor',owner='viewport',decision='REMOVED overload that ignored arguments; resource-bearing constructor retained'),
 dict(symbol='ViewportElement',owner='viewport UI',frequency='layout/update',decision='RETAIN output sizing, image display, resource lifetime; does not drive Simulation'),
 dict(symbol='CameraExtraction',owner='original RenderSystem',frequency='original reactive stage',decision='UNCHANGED ECS/GPU producer; no direct Editor backend path'),
 dict(symbol='currentImageExtent',owner='viewport',frequency='pick/drop only',decision='VALIDATE borrow, camera/request, adopted/published revision, receipt, sampled output identity and evidence; never reuse across callback/wait')]
e['c7']['dependency_tests']='c8-cxx-final-tests: 54/54 at f5447a763fd042f4f08819c8c88f1676f9a2a3cb; all/no-work/install recorded'
e['c8']['implementation_sha']=sha
e['c8']['first_failures'] += [x for x in [
 'c8-quality-build: partial-range automatic brace formatting broke a scope; reverted formatting-only changes and applied token-preserving full-file formatting, retained patch',
 'c8-quality-regression: fixed 32 UI iterations did not guarantee sampled camera output; fixture waits on NOT_READY only with unchanged-source/no-admission assertions; real Application passed'] if x not in e['c8']['first_failures']]
e['batches']['C8']={'status':'IMPLEMENTED_FINAL_QUALIFICATION_RUNNING','implementation_sha':sha}
e['batches']['C9']={'status':'IN_PROGRESS','implementation_sha':sha,'build':'independent clean tracked clone; EC3 + STRICT'}
e['next_entry']='C9 final Windows/PLAYER/installed SDK/header/actual GPU qualification; native input remains USER_DEFERRED'
p.write_text(json.dumps(j,ensure_ascii=False,indent=2)+'\n')
print('mapped',len(changes),'actual changed files;',len(e['application_methods']),'method decisions')
