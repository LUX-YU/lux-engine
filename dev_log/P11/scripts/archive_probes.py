"""Move real frozen P11 evidence, then reject missing/tampered evidence and a false source SHA."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib, json, shutil, subprocess, sys

w = Path(__file__).resolve().parent
s = Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
a = s / 'dev_log/P11'
sha = json.loads((a / 'receipt.json').read_text())['implementation_sha']
relocated = s.parent / 'build' / ('P11 归档迁移 ' + sha[:12])
assert not relocated.exists()
shutil.copytree(a, relocated)
out = a / 'archive-probes'
out.mkdir(exist_ok=True)
records = []

def run(name, archive, expected):
    args = [sys.executable, archive / 'verify.py', '--source', s, '--archive', archive]
    args = list(map(str, args))
    log = out / (name + '.log')
    assert not log.exists()
    start = datetime.now(timezone.utc).isoformat()
    with log.open('wb') as stream:
        result = subprocess.run(args, stdout=stream, stderr=subprocess.STDOUT)
    records.append(dict(name=name, argv=args, started=start, ended=datetime.now(timezone.utc).isoformat(),
                        expected=expected, exit_code=result.returncode, log=log.name,
                        sha256=hashlib.sha256(log.read_bytes()).hexdigest()))
    (out / 'results.json').write_text(json.dumps(records, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(name, result.returncode)
    assert (result.returncode == 0) == (expected == 0), log.read_text(errors='replace')[-7000:]

run('relocated-positive', relocated, 0)
target = relocated / 'logs/ctest.log'
original = target.read_bytes()
target.unlink() # The exact copied evidence file, never a source/build input.
run('missing-real-evidence', relocated, 1)
target.write_bytes(original + b'\nTAMPERED ACTUAL REGRESSION OUTPUT\n')
run('tampered-real-evidence', relocated, 1)
target.write_bytes(original)
receipt = relocated / 'receipt.json'
original_receipt = receipt.read_bytes()
data = json.loads(original_receipt)
data['implementation_sha'] = data['input_sha']
receipt.write_text(json.dumps(data), encoding='utf-8')
manifest = relocated / 'artifacts.json'
original_manifest = manifest.read_bytes()
entries = json.loads(original_manifest)
for item in entries:
    if item['path'] == 'receipt.json':
        item['sha256'] = hashlib.sha256(receipt.read_bytes()).hexdigest()
manifest.write_text(json.dumps(entries), encoding='utf-8')
# Hashes are now self-consistent: rejection must come from fixed source/receipt qualification.
run('false-source-sha', relocated, 1)
receipt.write_bytes(original_receipt)
manifest.write_bytes(original_manifest)
run('restored-relocated-positive', relocated, 0)
run('canonical-positive', a, 0)

# The probe logs are newly frozen evidence; they do not alter tested logs or receipts.
artifacts = [{'path': str(p.relative_to(a)).replace('\\', '/'),
              'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
             for p in sorted(a.rglob('*')) if p.is_file() and p.name != 'artifacts.json']
(a / 'artifacts.json').write_text(json.dumps(artifacts, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print('PASS archived relocation, two actual evidence failures, false SHA and restored positives')
