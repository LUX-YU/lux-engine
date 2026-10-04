from pathlib import Path
import hashlib, json, shutil, subprocess, sys
w=Path(__file__).resolve().parent
c=json.loads((w/'final-config.json').read_text())
s=Path(c['source']); p=Path(c['prefix']); base=s.parent/'build/RelWithDebInfo'/('ec3-sdk-'+c['implementation_sha'][:12])
def run(name, argv, expected=0):
    argv=list(map(str,argv))
    old=next((r for r in json.loads((w/'commands.json').read_text()) if r['name']==name),None)
    if old:
        assert old['argv']==argv and old['exit_code']==expected and old['source_head']==c['implementation_sha'],name
        assert hashlib.sha256((w/old['log']).read_bytes()).hexdigest()==old['sha256']
        print(name,'verified prior exact-command evidence',flush=True)
        return
    result=subprocess.run([sys.executable,str(w/'run.py'),'--runtime',str(p/'bin'),'--cwd',str(s),name,*argv])
    assert result.returncode==expected,name
common=['-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_PREFIX_PATH='+p.as_posix(),
    '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
    '-DLuxLua55_DIR=E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/cmake/LuxLua55',
    '-DPython3_EXECUTABLE='+sys.executable,'-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
run('final-sdk-regression',[sys.executable,s/'dev_log/P12/scripts/run_sdk.py','--source',s,'--prefix',p,
    '--output',w/'sdk','--build-root',base,'--only',
    'p11,p11-runtime,p11-models,quality,sessions,editor-d2,external-feature,scene-ui,scene-model,material-model,'
    'flowforge-model,persistence,scene-execution,projection-compilation,interaction-views,workspace,desktop-views,'
    'layering-tasks,layering-save_core,layering-project,layering-layout,gpu-ui,editor-scene-pane'])
for name, folder, extra in [
    ('declarations','editor-ec3-declarations',['-DEC3_MEASURE_COMMANDS=ON']),
    ('project-save','editor-ec3-project-save',[]),('workspace','editor-ec3-workspace',[]),
    ('skeleton','editor-ec1-skeleton',['-DEC1_MODE=HEADLESS','-DSDK_PREFIX='+p.as_posix()])]:
    build=base/('ec3-'+name)
    run('final-sdk-'+name+'-configure',['cmake','-S',s/'cmake/installed-consumers'/folder,'-B',build,*common,*extra])
    for step in ['build','no-work']:
        run('final-sdk-'+name+'-'+step,['cmake','--build',build,'--target','all','-j','4','--','-k','0'])
    if name=='declarations':
        for executable in ['editor_ec3_declarations','editor_ec3_measure']:
            run('final-sdk-'+executable,[build/(executable+'.exe')])
    else:
        run('final-sdk-'+name+'-tests',['ctest','--test-dir',build,'--output-on-failure','-j','1'])
        shutil.copy2(build/'Testing/Temporary/LastTest.log',w/('final-sdk-'+name+'-details.log'))
    if name=='skeleton':
        run('final-sdk-skeleton-window',[build/'bin/skeleton_consumer.exe',build,p,'WINDOW'])
for number in range(1,10):
    build=base/('declaration-negative-'+str(number))
    run('final-sdk-declaration-negative-'+str(number)+'-configure',
        ['cmake','-S',s/'cmake/installed-consumers/editor-ec3-declarations','-B',build,*common,
         '-DEC3_INVALID_DECLARATION='+str(number)])
    name='final-sdk-declaration-negative-'+str(number)+'-build'
    run(name,['cmake','--build',build,'--target','all','-j','4','--','-k','0'],expected=1)
    text=(w/'logs'/(name+'.log')).read_text(errors='replace')
    assert any(value in text for value in ['static_assert','constant expression','template argument','private member','C2672','C2131','C2248']),name
    assert 'C1083' not in text and 'LNK' not in text,name
run('final-sdk-codegen',[sys.executable,s/'cmake/installed-consumers/editor-inspector-generation/qualify.py',
    '--sdk',p,'--work',w/'Final SDK 中文 空格',
    *['--cmake-arg='+value for value in common if value.startswith('-D') and not value.startswith('-DCMAKE_PREFIX_PATH=')]])
