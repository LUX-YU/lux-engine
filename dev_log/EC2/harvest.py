from pathlib import Path
import hashlib, json, shutil
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text());b=Path(c['build'])
out=w/'build-proof';out.mkdir(exist_ok=True)
files=set()
for pattern in ['editor/tests/architecture/*boundaries.json','editor/tests/architecture/*-evidence.json',
    'editor/tests/architecture/**/reject_*.log','editor/tests/architecture/**/*negative*.log',
    'editor/tests/architecture/**/results.json','editor/tests/architecture/**/positive*.log',
    'editor/tests/architecture/**/repaired*.log','*-boundaries/results.json','*-boundaries/*/*.log',
    'layering-compiler/*.json','layering-compiler/**/*.log','delivery-constraints/*.json','delivery-constraints/**/*.log']:
    files.update(p for p in b.glob(pattern) if p.is_file())
for p in files:
    dest=out/p.relative_to(b);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
cache=(b/'CMakeCache.txt').read_text()
(out/'configuration.txt').write_text('\n'.join(x for x in cache.splitlines() if x.startswith(
    ('LUX_','CMAKE_BUILD_TYPE:','CMAKE_CXX_COMPILER:','CMAKE_PREFIX_PATH:','CMAKE_INSTALL_PREFIX:')))+'\n')
(out/'index.json').write_text(json.dumps([dict(path=p.relative_to(b).as_posix(),
    sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(files)],indent=2)+'\n')
reply=b/'.cmake/api/v1/reply'
shutil.copytree(reply,out/'file-api',dirs_exist_ok=True)
for project in ['ec2-final-native-sdk','ec2-final-skeleton-headless','ec2-final-skeleton-window',
                'ec2-final-skeleton-app','ec2-final-stale-install-probe','ec2-final-script-views',
                'ec2-final-external-artifact']:
    root=b.with_name(project)
    for pattern in ['**/Testing/Temporary/LastTest.log','**/constraints/*/*.log']:
        for p in root.glob(pattern):
            dest=out/'sdk-details'/project/p.relative_to(root)
            dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
print('EC2 build/SDK/generated proof harvested',len(files))
