from pathlib import Path
import json, shutil, hashlib
w=Path(__file__).resolve().parent
b=Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/ec1-137b8f441faa')
o=w/'build-proof';o.mkdir(exist_ok=True)
files=[]
for pattern in ['editor/tests/architecture/*boundaries.json','editor/tests/architecture/*-evidence.json',
                'editor/tests/architecture/**/reject_*.log','editor/tests/architecture/**/*negative*.log']:
    for p in b.glob(pattern):
        if p.is_file() and p not in files:files.append(p)
for p in files:
    rel=p.relative_to(b);dest=o/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
cache=(b/'CMakeCache.txt').read_text()
(o/'configuration.txt').write_text('\n'.join(line for line in cache.splitlines() if line.startswith(('LUX_','CMAKE_BUILD_TYPE:','CMAKE_CXX_COMPILER:','CMAKE_PREFIX_PATH:','CMAKE_INSTALL_PREFIX:')))+'\n')
(o/'index.json').write_text(json.dumps([dict(path=p.relative_to(b).as_posix(),sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in files],indent=2)+'\n')
print('archived build qualification details',len(files))
