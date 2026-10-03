from pathlib import Path
import hashlib,json,re,subprocess
w=Path(__file__).resolve().parent
p=Path('E:/SyncForder/CodeRepos/install/EC1-137b8f441faa')
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec1')
retired=re.compile(r'\b(?:PreparedSessionData|EProjectAssetKind|VCompiledSource|Bone_t)\b|EditHistory::(?:execute|undo|redo|clear|close)\b')
residual=[]
for file in (p/'include/lux/engine').rglob('*.hpp'):
    for number,line in enumerate(file.read_text(errors='replace').splitlines(),1):
        if retired.search(line):residual.append(dict(path=str(file.relative_to(p)),line=number,text=line))
assert not residual,residual
sync=json.loads((w/'module-header-sync.json').read_text())
result=dict(prefix=str(p),retired_residual=residual,implementation_sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=s,text=True).strip(),
            sdk_identity_files=[dict(path=str(v.relative_to(p)),sha256=hashlib.sha256(v.read_bytes()).hexdigest()) for v in p.rglob('*SdkIdentity*') if v.is_file()])
(w/'installed-audit.json').write_text(json.dumps(result,indent=2)+'\n')
print('Installed SDK old API/type residual scan PASS; prefix',p)
