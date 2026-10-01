from pathlib import Path
import json, hashlib
w=Path(__file__).resolve().parent
manifest=json.loads((w.parent/'layering/dependency-seed-source.json').read_text())
source=Path(manifest['source'])
prefix=Path(r'E:/SyncForder/CodeRepos/install/P11-dev')
count=0
for item in manifest['files']:
 data=(source/item['path']).read_bytes()
 assert hashlib.sha256(data).hexdigest()==item['sha256']
 if item['external_import_prefix_relocated']:
  data=data.replace(source.as_posix().encode(),prefix.as_posix().encode()).replace(str(source).encode(),str(prefix).encode())
 target=prefix/item['path']
 if target.exists():assert target.read_bytes()==data,str(target)
 else:
  target.parent.mkdir(parents=True,exist_ok=True)
  target.write_bytes(data)
 count+=1
print('External-only verified seed files',count)
