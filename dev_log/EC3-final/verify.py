"""Verify EC3 continuation from archive-relative bytes and fixed Git objects only."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--repo', type=Path, required=True)
parser.add_argument('--archive', type=Path, default=Path(__file__).resolve().parent)
args = parser.parse_args()
root = args.archive.resolve()
load = lambda path: json.loads((root / path).read_text(encoding='utf-8'))
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
manifest = load('manifest.json')
for name, expected in manifest.items():
    path = (root / name).resolve()
    assert path.is_relative_to(root) and path.is_file(), 'missing evidence: ' + name
    assert digest(path) == expected, 'changed evidence: ' + name
r = load('receipt.json')
assert r['stage'] == 'EC3' and r['architecture_mode'] == 'STRICT'
assert r['status'] == 'EC3_WINDOWS_SCOPE_COMPLETE_AWAITING_REVIEW'
assert not r['user_patch_applied'] and not r['main_modified'] and not r['agent_security_changes']
assert r['inherited_scope']['EC2_native_input'] == 'NOT_RUN_USER_DEFERRED'
assert r['inherited_scope']['P12'] == 'PARTIAL_USER_WAIVER'

def git(*argv):
    return subprocess.check_output(['git', *argv], cwd=args.repo).decode().strip()

def archived(name, path):
    info = r['prior_archive_trees'][name]
    return subprocess.check_output(['git', 'show', info['commit'] + ':dev_log/' + name + '/' + path],
                                   cwd=args.repo)

for name, item in r['prior_archive_trees'].items():
    assert git('rev-parse', item['commit'] + ':dev_log/' + name) == item['tree']
subprocess.run(['git', 'merge-base', '--is-ancestor', r['previous_implementation_sha'], r['implementation_sha']],
               cwd=args.repo, check=True)
changed = git('diff', '--name-only', r['previous_implementation_sha'], r['implementation_sha'],
              '--', ':(exclude)dev_log').splitlines()
assert sorted(changed) == ['cmake/installed-consumers/editor-ec3-project-save/main.cpp',
                           'editor/activities/project/CMakeLists.txt']
old = json.loads(archived('EC3-C9', 'receipt.json'))
assert r['inherited_results'] == old['source_and_player_results']
assert r['inherited_public_headers'] == old['public_headers']
assert old['source_and_player_results']['resumed-ctest']['count'] == 241
assert old['source_and_player_results']['final-player-tests']['count'] == 20
for name, item in r['inherited_results'].items():
    text = archived('EC3-C9', item['log']).decode(errors='replace')
    assert re.findall(r'Test\s+#\d+:\s+(\S+)\s+\.+\s+Passed', text) == item['passed']

records = load('commands.json')
by_name = {x['name']: x for x in records}
assert len(records) == len(by_name) == r['command_count']
failures = {f'final-sdk-declaration-negative-{n}-build': 1 for n in range(1, 10)}
failures['final-sdk-run-controls-build'] = 1
for record in records:
    assert record['exit_code'] == failures.get(record['name'], 0), record['name']
    assert record['source_head'] == record['source_head_after'] == r['implementation_sha']
    assert record['source_diff_sha256'] == hashlib.sha256(b'').hexdigest()
    assert not record['source_untracked_sha256']
    assert digest(root / record['log']) == record['sha256']
    if record['name'].endswith('-no-work'):
        assert 'no work to do' in (root / record['log']).read_text()
assert 'No tests were found' in (root / by_name['final-sdk-workspace-tests']['log']).read_text()
assert 'PASS installed WorkspaceChanges/Actions' in (root / by_name['final-sdk-workspace-executable']['log']).read_text()
assert 'C2039' in (root / by_name['final-sdk-run-controls-build']['log']).read_text()
assert 'EC3 SDK actual generated Run control:' in (root / 'final-sdk-run-controls-details.log').read_text()
for n in range(1, 10):
    text = (root / by_name[f'final-sdk-declaration-negative-{n}-build']['log']).read_text()
    assert any(code in text for code in ['C2672', 'C2131', 'C2248', 'static_assert'])
    assert 'C1083' not in text and 'LNK' not in text
for record in load('sdk/commands.json'):
    assert record['exit_code'] == 0
    assert digest(root / 'sdk' / record['log']) == record['sha256']
    if record['name'].endswith('-no-work'):
        assert 'no work to do' in (root / 'sdk' / record['log']).read_text()
for item in r['sdk_results'] + r['extra_results']:
    names = re.findall(r'Test\s+#\d+:\s+(\S+)\s+\.+\s+Passed',
                        (root / item['log']).read_text(errors='replace'))
    assert names == item['passed'] and len(names) == item['count'] and names
    assert not any('native_input' in name for name in names)
negatives = sorted((root / 'sdk').glob('projection-compilation-reject_*.log'))
assert len(negatives) == r['operation_compile_rejections'] == 8
for file in negatives:
    text = file.read_text(errors='replace')
    assert 'deleted' in text and 'C1083' not in text and 'LNK' not in text
events = load('Final SDK 中文 空格/events.json')
assert len(events) == 25
expected_failures = {'template-failure', 'formatter-failure', 'publication-failure'}
for event in events:
    assert event['exit_code'] == (1 if event['label'] in expected_failures else 0)
    assert digest(root / 'Final SDK 中文 空格' / (event['label'] + '.log')) == event['log_sha256']
assert next(e for e in events if e['label'] == 'final-no-change')['inspector_jobs'] == 0
assert len(load('coverage.json')) == 60
for topic in load('coverage.json'):
    for evidence in topic['execution']['evidence']:
        if evidence.startswith('prior:'):
            prior, rest = evidence.removeprefix('prior:').split('/', 1)
            if rest.startswith('resumed-ctest/'):
                assert rest.split('/', 1)[1] in r['inherited_results']['resumed-ctest']['passed']
            else:
                assert archived(prior, rest)
        else:
            assert (root / evidence).is_file(), evidence
audit = load('final-sdk-audit.json')
assert audit['implementation_sha'] == r['implementation_sha']
assert not audit['missing'] and not audit['old_emitter_install_hits']
assert audit['scene_composition']['sha256'] == load('sdk-restored-baseline.json')['sha256']
assert load('protection.json')['patch_sha256'] == r['user_patch_sha256']
cache = load('protection.json')['qualification_cache']
assert cache['LUX_EDITOR_MIGRATION_STAGE'] == 'EC3' and cache['LUX_EDITOR_LAYERING_MODE'] == 'STRICT'
print('Verified EC3 Windows-scoped continuation, genuine compile negatives and retained historical/deferred scope.')
