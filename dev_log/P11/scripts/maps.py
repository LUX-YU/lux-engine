from pathlib import Path
import json,re,subprocess
w=Path(r'E:/SyncForder/CodeRepos/lux-engine/.internal/editor-redesign/P11');s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
commands=[]
def add(old,label,menu,key,policy,final,owner,phase='P11'):
 commands.append(dict(old_id=old,label=label,menu=menu,shortcut=key,query='Pure availability on fixed target; no mutation',execute_target=policy,formal=final,owner=owner,phase=phase))
for old,label,key,new in [('lux.asset.save','Save','Ctrl+S','lux.editor.save'),('lux.edit.undo','Undo','Ctrl+Z','lux.editor.undo'),('lux.edit.redo','Redo','Ctrl+Y','lux.editor.redo')]:
 add(old,label,'File' if label=='Save' else 'Edit',key,'original SessionId; Save freezes at actual admission; history through installed role',new,'activities: SaveService / InstalledSession / concrete Session')
for old,label,menu,key,policy,closure in [
 ('lux.asset.save-as','Save As','File','Ctrl+Shift+S','fixed SessionId + prepared target','P12-B: target chooser + existing SaveService rebind'),
 ('lux.asset.save-all','Save All','File','','fixed accepted content IDs','P12-B/E: application bounded SaveAll'),
 ('lux.window.close','Close Window','File','Ctrl+W','complete ViewId generation','P12-D/E: ViewHost close + explicit last content decision'),
 ('lux.application.exit','Exit','File','','application close decision','P12-E: application exit draining'),
 ('lux.edit.cut/copy/paste/delete/select-all','Selection operations','Edit','','original domain selection; no fallback focus','P12-D: concrete interaction and clipboard semantics'),
 ('lux.window.create/<type>','Open Window','Window','','fixed ViewType + typed initial binding','P12-D: ViewFactorySnapshot and Host'),
 ('lux.window.show/<id>','Show Window','Window','','fixed ViewId','P12-D: Host show/focus'),
 ('lux.workspace.default/apply/<name>','Layouts','Window','','stable LayoutId plan + explicit provider snapshot','P12-C: workspace load/apply'),
 ('lux.product.refresh/default','Refresh/open initial scene','','','project identity + fixed initial locator','P12-B/F: explicit application startup'),
 ('lux.product.new-project','New Project','File','','new project config','P12-F: formal project creation view'),
 ('lux.product.open-project/recent/<n>','Open/Recent Projects','File','','owned project path, not index into changing list','P12-B/F: process launch and recent preferences'),
 ('lux.product.new-scene/new-material/new-flow','New Asset','File/New Asset','','new typed source factory','P12-B/D: creation workflows'),
 ('lux.product.settings','Settings / Layouts','Edit','','typed settings view','P12-D: workbench settings'),
 ('lux.product.open-asset','Open Asset','File','','fixed AssetReference','P12-B: OpenAndShow composition'),
 ('lux.product.about','Version','Help','','pure display','P12-F: application product info')]:
 add(old,label,menu,key,policy,closure,'old executable last consumer; no Unsupported placeholder added to formal menu','P12')
(w/'command-map.json').write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n')
features=[
 ('X11-01','query local strong pin, rejected direct publication, callback removes external handle','editor.commands; editor.contributions; baseline current C03'),
 ('X11-02','fixed contribution snapshot across factories; notification enqueue next batch; rejected candidate order','editor.contributions; installed.p11.extension.*'),
 ('X11-03','real Root menu/shortcut fixes A; focus B/catalog replaced; A closes and slot reused','editor.sessions.installation; installed.p11.models'),
 ('X11-04','three real worker decodes, owner History construction, hidden installation and role rollback','editor.sessions.installation; installed.p11.models; 8 boundaries plus three IO negatives'),
 ('X11-05','actual DLL final source/view/config/weak control block/code disposal; rejected callback exception','installed.p11.extension.with-configuration/without-configuration; frozen K SDK crash'),
 ('X11-06','fresh SDK external V7 DLL; five V6/version/size/fingerprint/count rejection DLLs','installed.p11.extension.*'),
 ('X11-07','Editor version 7->8 changes only Editor CMake identity; runtime-only installed plugin','check_abi.py; installed.p11.runtime'),
 ('X11-A','bounded registry/dispatch, recursive query/drain blocked; owner completion retained','editor.commands; editor.contributions; original P05-R1/R2 tests'),
 ('X11-B','Save sees new committed source; strict expected rejected; fixed deletion set survives selection change','editor.sessions.installation; installed.p11.models'),
 ('X11-C','duplicate/type/input/code mismatch, reflection metadata and read budget failures','editor.contributions; editor.sessions.installation; installed.p11.extension.*'),
 ('X11-D','view closes without content closure, late saved file published after content closure but not adopted','installed.p11.extension.*; original Run and TaskMonitor regressions')]
(w/'behavior-map.json').write_text(json.dumps([dict(id=i,contract=c,evidence=e,status='FINAL_PENDING') for i,c,e in features],indent=2)+'\n')
(w/'owner-map.json').write_text(json.dumps({
 'author_content':'SessionStore owns SceneSession/MaterialSession/FlowSession. They retain the unique History and SessionState.',
 'installation':'PreparedSessionInstallation owns reservation/roles while Store owns hidden session; InstalledSession owns roles only.',
 'save_registration':'SaveService constructor module allocates shared control blocks; revocable source and weak directory remain same service.',
 'save_and_disk':'SaveService/SaveExecution/WriteCoordinator/FileArtifactStore unchanged owner split; only prepared registration added.',
 'commands':'Registry immutable snapshots; dispatcher bounded invocation FIFO; entry/payload own their code. No task final state duplicate.',
 'contributions':'Application ContributionRegistry fixed pending batch and original reflection draft; no lower layer loads aggregate.',
 'views':'ViewFactory returns DetachedView; ViewHost sole top-level Pane owner, Runtime retirement unchanged.',
 'configuration':'ConfigurationValue owns RuntimeObject, code and shared reflection environment; ConfigurationEditor owns UI factory.',
 'runtime_plugin':'Existing engine project PluginLibrary retains runtime binary and dependencies; EditorExtension adds validated V7 table handle.',
 'weak_control_blocks':'Foreign catalog shared entries normalized in receiving module before validation; expired weak references do not keep DLL alive.'},indent=2)+'\n')
print('Command map',len(commands),'groups; behavior map',len(features))
