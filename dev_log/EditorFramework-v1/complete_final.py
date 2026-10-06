from pathlib import Path
import json,subprocess
root=Path('E:/SyncForder/CodeRepos/lux-engine'); record=root/'.internal/editor-redesign/framework-v1'
q=json.loads((record/'qualification.json').read_text())
source=Path(q['source']); build=Path(q['build']); sdk=Path(q['sdk'])
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
def run(name,*cmd,cwd=source,runtime=None):
 subprocess.run(['py','-3',str(record/'run.py'),'--cwd',str(cwd),'--runtime',str(runtime or build/'bin'),'closure-'+name,*map(str,cmd)],check=True)
assert not subprocess.check_output(['git','status','--porcelain'],cwd=source)
run('fetch','git','fetch',root,'codex/editor-framework-v1')
run('checkout','git','checkout','--detach',sha)
q['implementation_sha']=sha
q['cold_build_sha']='1d40785bb2dacb84fd96b79a726d4505c5c5484e'
q['final_change_scope']='Only legacy generated-header scope; default Editor/engine/modules unchanged. Reconfigured independent clean checkout and repeated all applicable qualification.'
(record/'qualification.json').write_text(json.dumps(q,indent=2)+'\n')
run('tracked','cmake',f'-DLUX_SOURCE_DIR={source}','-P',source/'cmake/ValidateTrackedSnapshot.cmake')
run('configure','cmake','-S',source,'-B',build)
run('all','cmake','--build',build,'--target','all','-j','4','--','-k','0')
run('second','cmake','--build',build,'--target','all','-j','4','--','-k','0')
run('ctest','ctest','--test-dir',build,'--output-on-failure')
assert not sdk.exists()
run('install','cmake','--install',build)
consumer=build.with_name(build.name+'-sdk-final')
run('sdk-configure','cmake','-S',source/'editor/tests/installed','-B',consumer,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
 '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
 f'-DCMAKE_PREFIX_PATH={sdk};E:/SyncForder/CodeRepos/install/EC3-recheck-5ec163ff8')
run('sdk-build','cmake','--build',consumer,'--target','all','-j','4','--','-k','0')
run('sdk-test','ctest','--test-dir',consumer,'--output-on-failure',runtime=sdk/'bin')
run('installed-product','powershell','-NoProfile','-File',record/'installed_product.ps1','-Prefix',sdk,runtime=sdk/'bin')
run('delivery','py','-3',record/'check_delivery.py')
development=Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine-framework')
run('legacy-second','cmake','--build',development,'--target','all','lux_editor_legacy','lux_launcher_legacy','editor_ui_presentation_legacy','sample_editor_test_legacy','-j','4','--','-k','0',cwd=root,runtime=development/'bin')
run('legacy-regression','ctest','--test-dir',development,'--output-on-failure','-R',r'^(framework\.|editor\.presentation|legacy\.plugin\.external_editor)',cwd=root,runtime=development/'bin')
legacy_sdk=Path('E:/SyncForder/CodeRepos/install/EditorFramework-v1-legacy-fixed')
assert not legacy_sdk.exists()
run('legacy-install','cmake','--install',development,'--prefix',legacy_sdk,cwd=root)
run('legacy-delivery','py','-3',record/'check_delivery.py',legacy_sdk,cwd=root)
# Same independent clean tree, regenerated PLAYER graph; reuse built foundation objects, not Editor targets.
run('player-configure','cmake','-S',source,'-B',build,'-DLUX_BUILD_PROFILE=PLAYER')
assert not any(s in (build/'build.ninja').read_text() for s in ['editor_legacy/','editor/CMakeFiles/','lux_editor.exe'])
run('player-all','cmake','--build',build,'--target','all','-j','4','--','-k','0')
run('player-second','cmake','--build',build,'--target','all','-j','4','--','-k','0')
run('player-test','ctest','--test-dir',build,'--output-on-failure')
print('PASS final qualified default Editor, clean SDK, legacy ON isolation, PLAYER, installed product and affected regression')
