"""Run the same compiled regression driver against separately identified old/new DLL sets."""
import hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
root=Path(sys.argv[1]).resolve()
out=root/(sys.argv[2] if len(sys.argv)>2 else 'creation-branches')
out.mkdir(parents=True,exist_ok=False)
runs=[]
for slot in ['d','l']:
    corrected=root/'images/corrected'/slot
    driver=Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w')/slot/'bin/simulation_script_lua_coroutine_integration_test.exe'
    driver_source=subprocess.check_output(['git','-C',
        'E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-region-opt/final-source','rev-parse','HEAD'],text=True).strip()
    isolated=out/slot
    isolated.mkdir()
    shutil.copy2(driver,isolated/driver.name)
    for revision in ['s0','corrected']:
        libraries=root/'images'/revision/slot
        env=dict(os.environ)
        env['PATH']=';'.join([str(libraries),'D:/Development/vcpkg/installed/x64-windows/bin',
            *[p for p in env['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]])
        for case,args in [('object',['--creation-oom','0']),('stack',['--creation-oom','1']),
                          ('registry',['--creation-registry-oom']),('retire',['--creation-retire']),
                          ('stop',['--creation-stop'])]:
            log=out/f'{slot}-{revision}-{case}.log'
            command=[str(isolated/driver.name),*args]
            with log.open('wb') as stream:
                code=subprocess.call(command,stdout=stream,stderr=subprocess.STDOUT,env=env)
            text=log.read_text()
            expected_success=revision=='corrected' or case=='stop'
            valid=(code==0)==expected_success
            if revision=='s0' and case in ['object','stack','registry']:
                valid=valid and code in [3796650500,86]
            if expected_success:
                valid=valid and ('CREATION_OOM_PASS' in text if case in ['object','stack','registry']
                    else 'CREATION_REENTRY_PASS' in text)
            runs.append(dict(vm_slot=slot,revision=revision,case=case,command=command,exit=code,
                expected_success=expected_success,valid=valid,
                driver_sha256=hashlib.sha256(driver.read_bytes()).hexdigest(),
                driver_source=driver_source,
                library_identity=json.loads((libraries/'identity.json').read_text())))
            (out/'runs.json').write_text(json.dumps(runs,indent=2))
            print(slot,revision,case,code,valid,flush=True)
print('BRANCH_RESULTS',all(run['valid'] for run in runs),flush=True)
if not all(run['valid'] for run in runs): raise SystemExit(1)
