"""Verify an EC3 partial checkpoint without accessing original production paths."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess

p = argparse.ArgumentParser()
p.add_argument('--archive', type=Path, default=Path(__file__).resolve().parent)
p.add_argument('--repo', type=Path, required=True)
args = p.parse_args()
a = args.archive.resolve()
manifest = json.loads((a/'manifest.json').read_text())
for name, digest in manifest.items():
    path = (a/name).resolve()
    assert path.is_relative_to(a) and path.is_file(), f'missing evidence: {name}'
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, f'changed evidence: {name}'
r = json.loads((a/'receipt.json').read_text())
assert r['status'] == 'PARTIAL_VALIDATION_BLOCKED' and r['push_requested_not_test_waiver']
assert not r['user_patch_applied'] and not r['main_modified']
assert r['inherited']['EC2_native_input'] == 'NOT_RUN_USER_DEFERRED'
sha = r['implementation_sha']
subprocess.run(['git','merge-base','--is-ancestor',r['baseline_sha'],sha],cwd=args.repo,check=True)
for rev in [r['baseline_sha'],sha]:
    tree = subprocess.check_output(['git','rev-parse',rev+':dev_log'],cwd=args.repo,text=True).strip()
    assert tree == r['original_dev_log_tree'], 'historical snapshots changed'
commands = {x['name']:x for x in json.loads((a/'commands.json').read_text())}
for x in commands.values():
    assert hashlib.sha256((a/x['log']).read_bytes()).hexdigest() == x['sha256'], x['name']
for name in ['final-tracked','final-configure','final-build','final-no-work','final-independent-boundaries']:
    x = commands[name]
    assert x['exit_code'] == 0 and x['source_head'] == sha and x['source_head_after'] == sha, name
    assert x['source_diff_sha256'] == hashlib.sha256(b'').hexdigest() and not x['source_untracked_sha256'], name
assert '-DLUX_EDITOR_MIGRATION_STAGE=EC3' in commands['final-configure']['argv']
assert '-DLUX_EDITOR_LAYERING_MODE=STRICT' in commands['final-configure']['argv']
assert 'no work to do' in (a/commands['final-no-work']['log']).read_text(errors='replace')
assert commands['final-ctest']['exit_code'] != 0
assert commands['final-unaffected-runtime']['exit_code'] == 8
assert 'GetLastError: 126' in (a/'logs/final-plugin-native-loader-error.log').read_text()
assert 'lux_engine_scene_composition.dll' in (a/'final-defender-events.txt').read_text()
report = json.loads((a/'final-partial-results.json').read_text())
assert not report['complete_ctest_pass'] and report['implementation_sha'] == sha
observed = set()
for name in ['final-ctest','final-independent-boundaries','final-unaffected-runtime']:
    text = (a/commands[name]['log']).read_text(errors='replace')
    observed.update(re.findall(r'Test\s+#\d+:\s+(\S+)\s+\.+\s+Passed',text))
assert observed == {name for name,t in report['tests'].items() if t['status']=='PASS'}
assert len(observed) == 231
assert report['counts'] == {'BLOCKED_QUARANTINED_DEPENDENCY':5,'FAILED_PLUGIN_LOAD':1,
    'NOT_RUN_USER_DEFERRED':1,'PASS':231,'TIMEOUT_BEFORE_MAIN_QUARANTINED_DEPENDENCY':4}
for entry in json.loads((a/'input/manifest.json').read_text())['files']:
    assert hashlib.sha256((a/'input'/entry['path']).read_bytes()).hexdigest() == entry['sha256']
snapshot = json.loads((a/'ledger-snapshot.json').read_text())
assert len(snapshot['issues']) == 149 and len(snapshot['coverage']) == 60
assert snapshot['push_checkpoint']['implementation_sha'] == sha
print('Verified authentic EC3 PARTIAL checkpoint; full behavior/SDK qualification is NOT complete:',sha)
