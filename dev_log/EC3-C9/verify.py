"""Verify this partial continuation using archived bytes and fixed Git objects only."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--repo', type=Path, required=True)
p.add_argument('--archive', type=Path, default=Path(__file__).resolve().parent)
a = p.parse_args()
root = a.archive.resolve()
manifest = json.loads((root / 'manifest.json').read_text())
for name, expected in manifest.items():
    file = (root / name).resolve()
    assert file.is_relative_to(root) and file.is_file(), f'missing evidence: {name}'
    assert hashlib.sha256(file.read_bytes()).hexdigest() == expected, f'changed evidence: {name}'
r = json.loads((root / 'receipt.json').read_text())
assert r['status'] == 'PARTIAL_SDK_QUARANTINED'
assert not r['user_patch_applied'] and not r['main_modified']
assert r['inherited']['EC2_native_input'] == 'NOT_RUN_USER_DEFERRED'
subprocess.run(['git', 'merge-base', '--is-ancestor', r['previous_implementation_sha'],
                r['implementation_sha']], cwd=a.repo, check=True)
tree = subprocess.check_output(['git', 'rev-parse', r['implementation_sha'] + ':dev_log/EC3'],
                               cwd=a.repo, text=True).strip()
assert tree == r['prior_archive_tree'], 'prior quarantine archive changed'
paths = subprocess.check_output(['git', 'diff', '--name-only', r['previous_implementation_sha'],
                                r['implementation_sha'], '--', ':(exclude)dev_log'],
                               cwd=a.repo, text=True).splitlines()
assert sorted(paths) == sorted(r['implementation_changes'])
records = json.loads((root / 'commands.json').read_text())
by_name = {x['name']: x for x in records}
assert len(records) == len(by_name)
failures = {'resumed-project-header-before': 2, 'resumed-declarations-configure': 1}
failures.update({f'resumed-declaration-negative-{n}-build': 1 for n in range(1, 10)})
for record in records:
    name = record['name']
    assert record['exit_code'] == failures.get(name, 0), name
    assert record['source_head'] == record['source_head_after'], name
    assert record['source_head'] in (r['previous_implementation_sha'], r['implementation_sha']), name
    # The review-worktree precheck returned PASS but recorded a nonempty raw-diff fingerprint.
    # Its bytes were not retained, so it is not used as clean-source qualification.
    if name != 'resumed-install-fix-tracked':
        assert record['source_diff_sha256'] == hashlib.sha256(b'').hexdigest(), name
    assert not record['source_untracked_sha256'], name
    assert hashlib.sha256((root / record['log']).read_bytes()).hexdigest() == record['sha256'], name
assert by_name['resumed-qualified-tracked']['source_head'] == r['implementation_sha']
for name, result in r['source_and_player_results'].items():
    record = by_name[name]
    text = (root / record['log']).read_text(errors='replace')
    names = re.findall(r'Test\s+#\d+:\s+(\S+)\s+\.+\s+Passed', text)
    assert names == result['passed'] and len(names) == result['count']
    assert result['implementation_sha'] == record['source_head']
for name in ['resumed-install-fix-no-work', 'final-player-no-work', 'resumed-project-consumer-no-work']:
    assert 'no work to do' in (root / by_name[name]['log']).read_text()
assert 'C1083' in (root / by_name['resumed-project-header-before']['log']).read_text()
assert 'ProjectCommands.hpp' in (root / by_name['resumed-install-fix-headers']['log']).read_text()
for n in range(1, 10):
    text = (root / by_name[f'resumed-declaration-negative-{n}-build']['log']).read_text()
    assert any(v in text for v in ['C2672', 'C2131', 'C2248', 'static_assert'])
    assert 'C1083' not in text and 'LNK' not in text
headers = json.loads((root / 'public-headers/commands.json').read_text())
assert len(headers) == 56 and all(x['exit_code'] == 0 for x in headers)
for item in headers:
    assert item['implementation_sha'] == r['implementation_sha']
    assert hashlib.sha256((root / 'public-headers' / item['log']).read_bytes()).hexdigest() == item['sha256']
missing = json.loads((root / 'resumed-install-first-audit.json').read_text())
assert len(missing['missing']) == 1 and missing['missing'][0].endswith('lux_engine_scene_composition.dll')
events = (root / 'resumed-sdk-defender-events.json').read_text(encoding='utf-8-sig')
assert 'lux_engine_scene_composition.dll' in events and '1117' in events
print('Verified EC3 C9 PARTIAL continuation; remaining SDK qualification is still blocked.')
