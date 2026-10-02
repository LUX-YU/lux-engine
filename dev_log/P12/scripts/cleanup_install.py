"""Exact owned-file cleanup in developer prefixes; historical qualification prefixes stay intact."""
from pathlib import Path
import argparse, hashlib, json, subprocess

w = Path(__file__).resolve().parent
s = Path('E:/SyncForder/CodeRepos/lux-engine-p12')
cluster = s.parent
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=s, text=True).strip()
fresh = cluster / 'install' / ('P12-' + sha[:12])
old_prefix = cluster / 'install/P11-1afb8f3f6e58'
old_manifest = cluster / 'build/RelWithDebInfo/p11-1afb8f3f6e58/install_manifest.txt'
new_manifest = cluster / 'build/RelWithDebInfo' / ('p12-' + sha[:12]) / 'install_manifest.txt'
p = argparse.ArgumentParser(); p.add_argument('--apply', action='store_true'); a = p.parse_args()

def relative_manifest(file, prefix):
    result = set()
    for line in file.read_text().splitlines():
        target = Path(line).resolve()
        assert target.is_relative_to(prefix.resolve()), line
        result.add(target.relative_to(prefix.resolve()).as_posix())
    return result

old = relative_manifest(old_manifest, old_prefix)
new = relative_manifest(new_manifest, fresh)
obsolete = old - new
prefixes = [cluster/'install/RelWithDebInfo', cluster/'install/Debug', cluster/'install/Android/lux-engine']
plan = []
for prefix in prefixes:
    for relative in sorted(obsolete):
        # Only first-party Engine artifacts from its own prior installation manifest.
        owned = relative.startswith(('include/lux/engine/', 'share/lux-engine'))
        owned |= relative.startswith(('bin/', 'lib/')) and ('editor' in Path(relative).name or 'lux_launcher' in relative)
        if not owned: continue
        target = (prefix/relative).resolve()
        assert target.is_relative_to(prefix.resolve()) and target != prefix.resolve()
        if target.is_file():
            plan.append(dict(prefix=str(prefix), relative=relative, path=str(target),
                             sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
# Include obsolete public headers from the exact source-provider comparison, including older developer installs.
for item in json.loads((w/'obsolete-headers-plan.json').read_text()):
    target = Path(item['path']).resolve(); prefix = Path(item['prefix']).resolve()
    assert prefix in [x.resolve() for x in prefixes] and target.is_relative_to(prefix)
    relative = target.relative_to(prefix).as_posix()
    assert relative not in new and relative.startswith('include/lux/engine/')
    if target.is_file() and all(x['path'].lower() != str(target).lower() for x in plan):
        plan.append(dict(prefix=str(prefix), relative=relative, path=str(target),
                         sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
record = dict(implementation_sha=sha, old_manifest_sha256=hashlib.sha256(old_manifest.read_bytes()).hexdigest(),
              new_manifest_sha256=hashlib.sha256(new_manifest.read_bytes()).hexdigest(), files=plan, applied=a.apply)
if a.apply:
    evidence=w/'installation-cleanup'; evidence.mkdir(exist_ok=True)
    assert not (evidence/'result.json').exists()
    (evidence/'result.json').write_text(json.dumps(record,indent=2),encoding='utf8')
    for index,item in enumerate(plan):
        target=Path(item['path']); prefix=Path(item['prefix']).resolve()
        assert target.resolve().is_relative_to(prefix) and target.is_file()
        data=target.read_bytes(); assert hashlib.sha256(data).hexdigest()==item['sha256']
        (evidence/(str(index)+'.original')).write_bytes(data)
        target.unlink()
        item['absent_after']=not target.exists()
        (evidence/'result.json').write_text(json.dumps(record,indent=2),encoding='utf8')
    (evidence/'result.json').write_text(json.dumps(record,indent=2),encoding='utf8')
else:
    (w/'installation-cleanup-plan.json').write_text(json.dumps(record,indent=2),encoding='utf8')
print('Controlled obsolete files:',len(plan),'applied:',a.apply)
