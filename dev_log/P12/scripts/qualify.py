"""Sequential qualification of one tracked P12 commit; never borrows developer Engine outputs."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess, sys
from datetime import datetime, timezone

p=argparse.ArgumentParser();p.add_argument('--phase',choices=['cold','player','cpu','sdk','clang','asan','regenerate','desktop','developer','input'],required=True)
a=p.parse_args();workspace=Path(r'E:/SyncForder/CodeRepos/lux-engine-p12');cluster=workspace.parent
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=workspace,text=True).strip()
work=Path(__file__).resolve().parent;out=work/'final'/sha;out.mkdir(parents=True,exist_ok=True)
repo=cluster/'build'/('p12-clean-'+sha[:12]);build=cluster/'build/RelWithDebInfo'/('p12-'+sha[:12])
prefix=cluster/'install'/('P12-'+sha[:12]);records_file=out/'commands.json'
records=json.loads(records_file.read_text()) if records_file.exists() else []
os.environ['PATH']=str(prefix/'bin')+os.pathsep+'D:/Development/vcpkg/installed/x64-windows/bin'+os.pathsep+os.pathsep.join(
    v for v in os.environ['PATH'].split(os.pathsep) if '/install/' not in v.replace('\\','/').lower() and '/lux-engine/bin' not in v.replace('\\','/').lower() and '/coderepos/build/' not in v.replace('\\','/').lower())

def run(name,args,cwd=repo,expected=0):
    args=list(map(str,args));prior=next((x for x in records if x['name']==name),None)
    if prior and prior['exit_code']==expected and prior['argv']==args:
        assert hashlib.sha256((out/prior['log']).read_bytes()).hexdigest()==prior['sha256']
        print(name,'verified same-commit evidence',flush=True);return
    suffix=1;log=out/(name+'.log')
    while log.exists():log=out/(name+f'-{suffix}.log');suffix+=1
    started=datetime.now(timezone.utc).isoformat()
    with log.open('wb') as stream:r=subprocess.run(args,cwd=cwd,stdout=stream,stderr=subprocess.STDOUT)
    ended=datetime.now(timezone.utc).isoformat()
    if prior:prior['name']+='-attempt-'+str(suffix-1)
    records.append({'name':name,'argv':args,'cwd':str(cwd),'exit_code':r.returncode,'log':log.name,
        'sha256':hashlib.sha256(log.read_bytes()).hexdigest(),'implementation_sha':sha,'started':started,'ended':ended})
    records_file.write_text(json.dumps(records,indent=2));print(name,r.returncode,flush=True)
    if r.returncode!=expected:print(log.read_text(errors='replace')[-10000:]);raise SystemExit(r.returncode or 1)

common=['-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',f'-DCMAKE_PREFIX_PATH={prefix.as_posix()}',
    f'-DCMAKE_INSTALL_PREFIX={prefix.as_posix()}','-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
    '-DVCPKG_TARGET_TRIPLET=x64-windows','-DBUILD_TESTING=ON','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    '-DLuxLua55_DIR=E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/cmake/LuxLua55',
    '-DPython3_EXECUTABLE='+sys.executable,
    '-DLUX_EDITOR_MIGRATION_STAGE=P12','-DLUX_EDITOR_LAYERING_MODE=STRICT','-DLUX_BUILD_COMPONENT_LUA_META=OFF',
    '-DLUX_BUILD_PHYSICS2D=ON','-DLUX_BUILD_PACKED_RENDER_CONTENT=ON',
    *['-DLUX_EDITOR_BUILD_'+group+'_TESTS=ON' for group in ['NATIVE','DESKTOP','GPU','TOOLCHAIN','INSTALLED']]]

if a.phase=='cold':
    assert not subprocess.check_output(['git','status','--porcelain'],cwd=workspace,text=True).strip(), 'Qualification requires clean implementation checkout'
    if not repo.exists():
        run('clone',['git','clone','--no-hardlinks','--no-checkout',workspace,repo],workspace)
        run('checkout',['git','checkout','--detach',sha])
    assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()==sha
    run('tracked-snapshot',['cmake',f'-DLUX_SOURCE_DIR={repo}','-P',repo/'cmake/ValidateTrackedSnapshot.cmake'])
    manifest=json.loads((work.parent/'layering/dependency-seed-source.json').read_text());original=Path(manifest['source'])
    copied=[]
    for item in manifest['files']:
        src=original/item['path'];dst=prefix/item['path'];dst.parent.mkdir(parents=True,exist_ok=True)
        data=src.read_bytes()
        assert hashlib.sha256(data).hexdigest()==item['sha256'], str(src)
        if item['external_import_prefix_relocated']:
            data=data.replace(original.as_posix().encode(),prefix.as_posix().encode()).replace(str(original).encode(),str(prefix).encode())
        if dst.exists():assert dst.read_bytes()==data
        else:dst.write_bytes(data)
        copied.append({'path':item['path'],'sha256':hashlib.sha256(data).hexdigest()})
    (out/'dependency-seed.json').write_text(json.dumps(copied,indent=2))
    query=build/'.cmake/api/v1/query';query.mkdir(parents=True,exist_ok=True)
    for name in ['codemodel-v2','cache-v2','toolchains-v1']:(query/name).touch()
    run('configure',['cmake','-S',repo,'-B',build,*common,'-DLUX_BUILD_PROFILE=EDITOR'])
    for name in ['build','no-work']:run(name,['cmake','--build',build,'--target','all','-j','4','--','-k','0'])
    run('test-names',['ctest','--test-dir',build,'--show-only=json-v1'])
    run('ctest',['ctest','--test-dir',build,'--output-on-failure','-j','1','-E','^editor.desktop_native_input$'])
    shutil.copyfile(build/'Testing/Temporary/LastTest.log',out/'ctest-details.log')
    run('product-help',[build/'bin/lux_editor.exe','--help'],build/'bin')
    run('launcher-smoke',[build/'bin/lux_launcher.exe','--smoke'],build/'bin')
    run('install',['cmake','--install',build,'--prefix',prefix])
    run('installed-editor-smoke',[sys.executable,work/'run_installed_product.py','--prefix',prefix,'--build',build,'--output',out/'installed-product'])
    path_build=build.with_name(build.name+'-long-path-sdk')
    run('long-path-sdk-configure',['cmake','-S',work/'path-regression','-B',path_build,*common])
    run('long-path-sdk-build',['cmake','--build',path_build,'--target','all','-j','4','--','-k','0'])
    previous=json.loads((work/'runs/commands.json').read_text())
    fixture=next(r['argv'][-1] for r in previous if r['name']=='h-path-before-run')
    run('long-path-sdk-run',[path_build/'native_path.exe',fixture])

    run('imports',['dumpbin','/dependents',build/'bin/lux_editor.exe'])
    run('actual-link-commands',['ninja','-C',build,'-t','commands'])
elif a.phase in ['player','cpu']:
    variant=build.with_name(build.name+'-'+a.phase)
    options=['-DLUX_BUILD_PROFILE=PLAYER'] if a.phase=='player' else ['-DLUX_BUILD_PROFILE=EDITOR',
        '-DLUX_EDITOR_BUILD_GPU_TESTS=OFF','-DLUX_EDITOR_BUILD_DESKTOP_TESTS=OFF','-DLUX_EDITOR_BUILD_TOOLCHAIN_TESTS=OFF',
        '-DLUX_EDITOR_BUILD_INSTALLED_TESTS=OFF','-DLUX_EDITOR_BUILD_NATIVE_TESTS=ON']
    run(a.phase+'-configure',['cmake','-S',repo,'-B',variant,*common,*options])
    for name in ['build','no-work']:run(a.phase+'-'+name,['cmake','--build',variant,'--target','all','-j','4','--','-k','0'])
    run(a.phase+'-ctest',['ctest','--test-dir',variant,'--output-on-failure','-j','1'])
    shutil.copyfile(variant/'Testing/Temporary/LastTest.log',out/(a.phase+'-ctest-details.log'))
    run(a.phase+'-test-names',['ctest','--test-dir',variant,'--show-only=json-v1'])
    if a.phase=='player':
        for unit in json.loads((variant/'compile_commands.json').read_text()):
            assert '/editor/' not in (unit['file']+' '+unit['command']).replace('\\','/').lower()
        run('player-imports',['dumpbin','/dependents',variant/'bin/engine_headless_test.exe'])
elif a.phase=='input':
    print('All non-input tests complete; awaiting the reserved native-input window.',flush=True)
    import time
    while not (work/'input-window-ready').exists() or (work/'input-window-ready').read_text().strip()!=sha: time.sleep(1)
    run('native-input',['ctest','--test-dir',build,'--output-on-failure','-j','1','-R','^editor.desktop_native_input$'])
    shutil.copyfile(build/'Testing/Temporary/LastTest.log',out/'native-input-details.log')
    installed=build.with_name(build.name+'-sdk')/'desktop-views'
    run('sdk-native-input',['ctest','--test-dir',installed,'--output-on-failure','-j','1','-R','^installed.desktop.native_input$'])
    shutil.copyfile(installed/'Testing/Temporary/LastTest.log',out/'sdk-native-input-details.log')
elif a.phase=='desktop':
    run('installed-menu',[sys.executable,work/'run_installed_product.py','--prefix',prefix,'--build',build,'--output',out/'installed-menu','--manual'])
elif a.phase=='developer':
    destination=cluster/'install/RelWithDebInfo'
    manifest=build/'install_manifest.txt';original=manifest.read_bytes()
    try:
        run('developer-install',['cmake','--install',build,'--prefix',destination])
        (out/'developer-install-manifest.txt').write_bytes(manifest.read_bytes())
    finally:
        manifest.write_bytes(original)
    run('developer-editor-smoke',[sys.executable,work/'run_installed_product.py','--prefix',destination,'--build',build,'--output',out/'developer-product'])
elif a.phase=='regenerate':
    folder=(build/'editor/workbench/scene/inspector_gen/scene_fields_transform').resolve()
    assert folder.is_relative_to(build.resolve()) and folder.is_dir()
    outputs=[p for p in folder.iterdir() if p.name.endswith(('.inspector.generated.cpp','.inspector.generated.hpp'))]
    assert len(outputs)==3
    before=[{'path':str(p.relative_to(build)), 'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in outputs]
    (out/'regeneration-before.json').write_text(json.dumps(before,indent=2))
    for p in outputs:
        assert p.resolve().is_relative_to(folder)
        p.unlink()
    for name in ['regenerate','regenerate-no-work']:
        run(name,['cmake','--build',build,'--target','all','-j','4','--','-k','0'])
    for item in before:
        assert hashlib.sha256((build/item['path']).read_bytes()).hexdigest()==item['sha256']
    (out/'regeneration-after.json').write_text(json.dumps(before,indent=2))
elif a.phase=='sdk':
    run('sdk-all',[sys.executable,work/'run_sdk.py','--source',repo,'--prefix',prefix,
        '--output',out/'sdk','--build-root',build.with_name(build.name+'-sdk')])
else:
    variant=build.with_name(build.name+'-'+a.phase)
    flags=[]
    if a.phase=='asan':
        flags=['-DCMAKE_CXX_FLAGS=/fsanitize=address',
               '-DCMAKE_EXE_LINKER_FLAGS=/LIBPATH:D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/lib/clang/19/lib/windows clang_rt.asan_dynamic-x86_64.lib clang_rt.asan_dynamic_runtime_thunk-x86_64.lib',
               '-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/DEBUG /INCREMENTAL:NO',
               '-DCMAKE_TRY_COMPILE_CONFIGURATION=RelWithDebInfo']
        os.environ['PATH']='D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/lib/clang/19/lib/windows'+os.pathsep+os.environ['PATH']
    run(a.phase+'-configure',['cmake','-S',repo/'cmake/installed-consumers/quality','-B',variant,*common,
        '-DCMAKE_CXX_COMPILER=D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/bin/clang-cl.exe',*flags])
    for name in ['build','no-work']:run(a.phase+'-'+name,['cmake','--build',variant,'--target','all','-j','4','--','-k','0'])
    run(a.phase+'-ctest',['ctest','--test-dir',variant,'--output-on-failure','-j','1'])
    shutil.copyfile(variant/'Testing/Temporary/LastTest.log',out/(a.phase+'-ctest-details.log'))
    if a.phase=='clang':
        run('clang-public-headers',[sys.executable,work/'check_headers.py'])
