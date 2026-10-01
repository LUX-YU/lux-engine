from pathlib import Path
import hashlib, json, shutil, subprocess, sys

w = Path(__file__).resolve().parent
source = Path('E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
archive = w / 'archive'
relocated = w / '归档迁移 含空格' / 'P10Q-structure'
assert not relocated.exists(), 'Use a fresh portable qualification destination; preserve prior evidence.'
shutil.copytree(archive, relocated)
probe = w / 'archive-probes'
probe.mkdir(exist_ok=True)
records = []
validator = source / 'editor/tests/architecture/validate_layering_evidence.py'

def run(name, folder, expected_success):
    log = probe / (name + '.log')
    assert not log.exists(), log
    args = [sys.executable, str(validator), '--source', str(source), '--archive', str(folder)]
    with log.open('wb') as output:
        result = subprocess.run(args, stdout=output, stderr=subprocess.STDOUT)
    records.append({'name': name, 'argv': args, 'exit_code': result.returncode,
                    'expected': 'PASS' if expected_success else 'REJECT', 'log': log.name,
                    'sha256': hashlib.sha256(log.read_bytes()).hexdigest()})
    (probe / 'results.json').write_text(json.dumps(records, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(name, result.returncode, flush=True)
    assert (result.returncode == 0) == expected_success, log.read_text(errors='replace')
    if expected_success:
        assert 'P10Q-structure archive verified: PASS' in log.read_text()

run('relocated-positive', relocated, True)
target = (relocated / 'logs/build.log').resolve()
assert target.is_relative_to(relocated.resolve()) and target.is_file()
original = target.read_bytes()
try:
    target.unlink()
    run('missing-real-evidence', relocated, False)
finally:
    target.write_bytes(original)
try:
    target.write_bytes(original + b'\nP10Q structure deliberate evidence tamper probe\n')
    run('tampered-real-evidence', relocated, False)
finally:
    target.write_bytes(original)
run('restored-relocated-positive', relocated, True)
run('canonical-final', archive, True)
assert all((x['exit_code'] == 0) == (x['expected'] == 'PASS') for x in records)
shutil.copytree(probe, archive / 'archive-probes')
manifest = [{'path': p.relative_to(archive).as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in sorted(archive.rglob('*')) if p.is_file() and p.name not in ['artifacts.json', 'receipt.json']]
(archive / 'artifacts.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
print('Portable, missing and tampered real evidence verified; probe output frozen in archive.', flush=True)
