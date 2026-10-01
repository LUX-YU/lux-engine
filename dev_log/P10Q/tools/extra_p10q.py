from pathlib import Path
import subprocess,os,json,hashlib,sys,re
repo=Path(__file__).resolve().parents[2];work=repo/'.internal/editor-redesign';cluster=repo.parent
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
build=cluster/'build/RelWithDebInfo'/('p10q-'+sha[:12]);prefix=cluster/'install'/('P10Q-'+sha[:12])
out=work/'P10Q-final'/sha/'extra';out.mkdir(parents=True,exist_ok=True)
records=json.loads((out/'commands.json').read_text()) if (out/'commands.json').exists() else []
os.environ['PATH']=str(prefix/'bin')+os.pathsep+'D:/Development/vcpkg/installed/x64-windows/bin'+os.pathsep+os.environ['PATH']
def run(name,args,cwd=repo,expected=0):
    args=list(map(str,args));log=out/(name+'.log');i=1
    prior=next((x for x in records if x['name']==name),None)
    if prior and prior['exit_code']==expected and prior['argv']==args:
        assert hashlib.sha256((out/prior['log']).read_bytes()).hexdigest()==prior['sha256']
        print(name,'verified same-input evidence',flush=True);return
    while log.exists():log=out/(name+f'-{i}.log');i+=1
    with log.open('wb') as stream:r=subprocess.run(args,cwd=cwd,stdout=stream,stderr=subprocess.STDOUT,timeout=180)
    if prior:prior['name']+='-attempt-'+str(i-1)
    records.append({'name':name,'argv':args,'exit_code':r.returncode,'log':log.name,'implementation_sha':sha,
                    'sha256':hashlib.sha256(log.read_bytes()).hexdigest()})
    (out/'commands.json').write_text(json.dumps(records,indent=2));print(name,r.returncode,flush=True)
    if r.returncode!=expected:raise SystemExit(r.returncode or 1)
run('inventory',[sys.executable,work/'p10q_inventory.py','--build',build,'--out',out/'file-api-inventory.json'])
cdb=Path('C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe')
run('editor-loaded-modules',[cdb,'-c','lmf;q',build/'bin/lux_editor.exe'])
before=cluster/'build/RelWithDebInfo/p05-r1-f2b5a00c60e9'
current_path=os.environ['PATH']
os.environ['PATH']=str(cluster/'install/RelWithDebInfo/bin')+os.pathsep+current_path
run('before-editor-loaded-modules',[cdb,'-c','lmf;q',before/'bin/lux_editor.exe'])
os.environ['PATH']=current_path
linker=Path('D:/Development/vcpkg/installed/x64-windows/tools/llvm/lld-link.exe')
if not linker.exists():
    import shutil
    linker=Path(shutil.which('lld-link') or 'D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/bin/lld-link.exe')
run('new-harness-loaded-modules',[cdb,'-g','-G',build/'bin/editor_scene_views_test.exe',linker,build],build/'bin')
# Concurrent processes deliberately share the parent while each real fixture owns a unique child.
base=build/'workspace-concurrency';base.mkdir(exist_ok=True)
pending=[]
for i,mode in enumerate(['catalog','catalog','rename','rename']):
    log=out/f'workspace-concurrent-{i}.log';stream=log.open('wb')
    argv=list(map(str,[build/'bin/editor_workspace_test.exe',base,mode]))
    pending.append((subprocess.Popen(argv,stdout=stream,stderr=subprocess.STDOUT),stream,log,argv))
paths=[]
for i,(process,stream,log,argv) in enumerate(pending):
    code=process.wait(timeout=90);stream.close();text=log.read_text(errors='replace')
    assert code==0,text
    paths+=re.findall(r'isolated workspace fixture: (.+)',text)
    records.append({'name':f'workspace-concurrent-{i}','argv':argv,'exit_code':code,'log':log.name,
        'implementation_sha':sha,'sha256':hashlib.sha256(log.read_bytes()).hexdigest()})
assert len(paths)==4 and len(set(paths))==4,paths
(out/'commands.json').write_text(json.dumps(records,indent=2))
print('four distinct concurrent real workspace fixtures PASS',flush=True)
patterns='ProjectCatalogAccess|ProjectCatalogAdapter|TaskQueryPort|EMaterialViewAction|MaterialViewRequests|FlowViewRequests|PublishCompiledMaterialOperation|PublishFlowArtifactOperation|ProjectArtifactStore|editor_project_io|lux/engine/editor/io/SaveExecution|lux/engine/editor/scene/SceneElement.hpp'
run('removed-symbols',['rg','-n',patterns,'editor','engine','modules','cmake/installed-consumers',
    '-g','*.cpp','-g','*.hpp','-g','CMakeLists.txt'],expected=1)
consumer_symbols='ProjectCatalogModel|TaskMonitor|MaterialCompilationService|FileArtifactStore|publishEncodedArtifact|ViewportElement|ViewportPresentation|editor_persistence_execution|editor_viewport|TSessionAccess<'
clean=cluster/'build'/('p10q-clean-'+sha[:12])
run('migrated-consumers',['rg','-n',consumer_symbols,'editor','engine','cmake/installed-consumers',
    '-g','*.cpp','-g','*.hpp','-g','CMakeLists.txt'],cwd=clean)
print('extra qualification complete',flush=True)
