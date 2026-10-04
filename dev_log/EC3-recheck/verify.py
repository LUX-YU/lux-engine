from pathlib import Path
import argparse, hashlib, json, subprocess

p = argparse.ArgumentParser(); p.add_argument('--repo', required=True); a = p.parse_args()
root = Path(__file__).resolve().parent
manifest = json.loads((root/'manifest.json').read_text())
for name, wanted in manifest.items():
    path = root/name
    assert path.resolve().is_relative_to(root.resolve()), name
    assert path.is_file(), 'Missing archive evidence: '+name
    assert hashlib.sha256(path.read_bytes()).hexdigest() == wanted, 'Tampered archive evidence: '+name
receipt = json.loads((root/'receipt.json').read_text())
before = json.loads((root/'before/identity.json').read_text())
assert hashlib.sha256((root/'before/settings_navigation.cpp').read_bytes()).hexdigest() == before['fixture_sha256']
for item in receipt['changed_source_blobs']:
    raw = subprocess.check_output(['git', 'show', receipt['implementation_sha']+':'+item['path']], cwd=a.repo)
    assert hashlib.sha256(raw).hexdigest() == item['sha256'], item['path']
by_name = {row['name']: row for row in receipt['commands']}
for row in receipt['commands']:
    assert hashlib.sha256((root/row['log']).read_bytes()).hexdigest() == row['sha256']
for name, expected in [('scope', 'first allowed page: draft=0'),
                       ('readonly', 'apply calls=1, applied=1, writes=0')]:
    row = by_name['recheck-before-'+name]
    assert row['exit_code'] != 0 and expected in (root/row['log']).read_text()
assert by_name['recheck-final-affected']['source_head'] == receipt['implementation_sha']
assert by_name['recheck-final-affected']['exit_code'] == 0
assert by_name['recheck-sdk-desktop-gpu']['exit_code'] == 0
assert 'out of 244' in (root/by_name['recheck-ctest']['log']).read_text()
print('Verified EC3 recheck: actual before failures, source-qualified repairs and retained scope.')
