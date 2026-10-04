from pathlib import Path
import hashlib, json, re, subprocess, sys

w = Path(__file__).resolve().parent
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=s, text=True).strip()
assert not subprocess.check_output(['git', 'status', '--porcelain'], cwd=s).strip()
subprocess.run([sys.executable, str(w/'run.py'), '--runtime', str(s.parent/'install/EC3-C8/bin'),
    'final-pre-clone-tracked', 'cmake', '-DLUX_SOURCE_DIR='+str(s),
    '-P', str(s/'cmake/ValidateTrackedSnapshot.cmake')], check=True)
q = s.with_name('lux-engine-ec3-qualified')
assert not q.exists()
subprocess.run(['git', 'clone', '--no-hardlinks', '--no-checkout', str(s), str(q)], check=True)
subprocess.run(['git', 'checkout', '--detach', sha], cwd=q, check=True)
assert not subprocess.check_output(['git', 'status', '--porcelain'], cwd=q).strip()
prefix = s.parent/'install'/('EC3-'+sha[:12])
assert not prefix.exists()
seed = json.loads((s/'dev_log/EC2-R1/development-dependency-seed.json').read_text())
source = Path(seed['source'])
records = {}
for item in seed['files']:
    raw = (source/item['path']).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == item['sha256'], item['path']
    target = prefix/item['path']
    if target.suffix == '.cmake':
        text = re.sub(r'E:/SyncForder/CodeRepos/install/EC1(?:-development|-137b8f441faa)',
                      prefix.as_posix(), raw.decode('utf-8-sig'))
        raw = text.encode()
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(raw)
    records[item['path']] = dict(source=str(source/item['path']),
        source_sha256=item['sha256'], sha256=hashlib.sha256(raw).hexdigest())

# Replace the complete installed lux-cxx package with the fixed EC3 parser/ABI, not just its headers.
cxx = s.parent/'install/EC3-cxx'
cxx_source = s.with_name('lux-cxx-ec3')
cxx_sha = subprocess.check_output(['git','rev-parse','HEAD'], cwd=cxx_source,text=True).strip()
assert not subprocess.check_output(['git','status','--porcelain'],cwd=cxx_source).strip()
for path in cxx.rglob('*'):
    if not path.is_file(): continue
    relative = path.relative_to(cxx)
    raw = path.read_bytes()
    source_hash = hashlib.sha256(raw).hexdigest()
    if path.suffix == '.cmake':
        raw = raw.decode('utf-8-sig').replace(cxx.as_posix(), prefix.as_posix()).encode()
    target = prefix/relative
    target.parent.mkdir(parents=True,exist_ok=True)
    target.write_bytes(raw)
    records[relative.as_posix()] = dict(source=str(path), source_sha256=source_hash,
        sha256=hashlib.sha256(raw).hexdigest(), dependency_sha=cxx_sha)
assert not (prefix/'include/lux/engine').exists(), 'No first-party Engine SDK seeded'
config = dict(implementation_sha=sha, dependency_sha=cxx_sha, source=str(q), review_source=str(s),
    build=str(s.parent/'build/RelWithDebInfo'/('ec3-'+sha[:12])), prefix=str(prefix))
(w/'final-config.json').write_text(json.dumps(config,indent=2)+'\n')
(w/'final-dependency-seed.json').write_text(json.dumps(records,indent=2)+'\n')
print(config)
