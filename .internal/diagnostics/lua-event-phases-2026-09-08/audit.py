"""Freeze actual profiled images and verify protected workspaces without modifying them."""
import datetime, hashlib, json, shutil, subprocess, sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
start = json.loads((root/'identity-start.json').read_text())
branch = Path(__file__).resolve().parents[3]
qualified = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-region-opt/final-source')
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
git = lambda p, *args: subprocess.check_output(['git','-C',str(p),*args])
result = {'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'qualified_source':git(qualified,'rev-parse','HEAD').decode().strip(),
          'analysis_branch_head':git(branch,'rev-parse','HEAD').decode().strip(),
          'qualified_status':git(qualified,'status','--porcelain').decode(),
          'production_diff':git(branch,'diff',start['qualified'],'HEAD','--','engine','modules').decode(),
          'images':[], 'protected':[], 'dependencies':[], 'pin_checks':[]}
assert result['qualified_status']=='' and result['production_diff']==''
for item in start['images']:
    path=Path(item['path']); sha=digest(path)
    dst=root/'images'/path.name; dst.parent.mkdir(exist_ok=True)
    if not dst.exists():shutil.copy2(path,dst)
    assert digest(dst)==sha
    result['images'].append({'path':str(path),'sha256':sha,
                            'same_as_before_all_build':sha==item['sha256'],
                            'saved':str(dst.relative_to(root))})
for item in start['protected']:
    same=digest(Path(item['path']))==item['sha256']
    result['protected'].append({**item,'unchanged':same});assert same
for item in start['dependencies']:
    if 'path' in item:
        current={'path':item['path'],'head':git(item['path'],'rev-parse','HEAD').decode().strip(),
                 'status':git(item['path'],'status','--porcelain').decode().strip()}
        assert current['head']==item['head'] and current['status']==item['status']
        result['dependencies'].append(current)
    else:
        prefix=Path(item['prefix'])
        for file in item['files']:assert digest(prefix/file['path'])==file['sha256']
        result['dependencies'].append({'prefix':str(prefix),'verified_unchanged_files':len(item['files'])})
for repository,pin,prefix,files in [
    ('E:/SyncForder/CodeRepos/lux-cxx','3100f54d0743c5ed94a4ccf5943df04e933de255',
     'E:/SyncForder/CodeRepos/install/q2/c',[
         'container/include/lux/cxx/container/SlotMap.hpp',
         'container/include/lux/cxx/container/StableSlotMap.hpp',
         'compile_time/include/lux/cxx/compile_time/expected.hpp'])]:
    for file in files:
        tracked=git(repository,'show',pin+':'+file)
        installed=Path(prefix)/('include/'+file.split('/include/')[1])
        same=tracked.replace(b'\r\n',b'\n')==installed.read_bytes().replace(b'\r\n',b'\n')
        result['pin_checks'].append({'pin':pin,'source':file,'installed':str(installed),
                                     'normalized_content_equal':same});assert same
artifact=Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w/t/engine/toolchain/lua/lua_runtime_benchmark_fixture.lxsa')
result['artifact']={'path':str(artifact),'sha256':digest(artifact)}
result['artifact_inputs']=[{'path':str(p.relative_to(qualified)),'sha256':digest(p)}
    for p in sorted((qualified/'engine/toolchain/lua/test').glob('lua_runtime_benchmark_fixture.*'))]
(root/'identity-final.json').write_text(json.dumps(result,indent=2))
print('AUDIT PASS: production source equal; main/dependencies unchanged; actual images preserved')
for p in result['images']:
    if not p['same_as_before_all_build']:print('REBUILT SAME SOURCE:',Path(p['path']).name,p['sha256'])
