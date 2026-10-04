from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import shutil
import subprocess
import sys

w = Path(__file__).resolve().parent
repo = Path(json.loads((w / 'final-config.json').read_text())['review_source'])
archive = repo / 'dev_log/EC3-final'
relocated = w / '最终归档核查 空格'
assert not relocated.exists()

def manifest():
    value = {p.relative_to(archive).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in sorted(archive.rglob('*')) if p.is_file() and p.name != 'manifest.json'}
    (archive / 'manifest.json').write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

manifest()
shutil.copytree(archive, relocated)
checks = []
def run(name, target, expected, message=None):
    argv = [sys.executable, str(target / 'verify.py'), '--repo', str(repo), '--archive', str(target)]
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = result.stdout.decode(errors='replace')
    assert result.returncode == expected, output
    if message:
        assert message in output, output
    checks.append({'case': name, 'argv': argv, 'exit_code': result.returncode,
                   'at_utc': datetime.now(timezone.utc).isoformat(), 'output': output})

for name in ['EC3','EC3-C9']:
    run('prior-' + name, repo / 'dev_log' / name, 0)
run('original', archive, 0)
run('relocated-chinese-space', relocated, 0)
victim = (relocated / 'logs/final-sdk-workspace-executable.log').resolve()
assert victim.is_relative_to(relocated.resolve()) and victim.is_file()
original = victim.read_bytes()
try:
    victim.unlink()
    run('missing-real-log', relocated, 1, 'missing evidence:')
    victim.write_bytes(original + b'\nchanged evidence\n')
    run('changed-real-log', relocated, 1, 'changed evidence:')
finally:
    victim.write_bytes(original)
run('restored', relocated, 0)
(archive / 'archive-checks.json').write_text(json.dumps(checks, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
shutil.copy2(__file__, archive / 'check_final_archive.py')
manifest()
result = subprocess.run([sys.executable, str(archive / 'verify.py'), '--repo', str(repo)], check=True)
print('Archive positive, relocated, genuine missing/tamper negative and historical checks completed.')
