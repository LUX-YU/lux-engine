"""Freeze P11 evidence only after the same clean implementation passes its required matrix."""
from pathlib import Path
from datetime import datetime, timezone
import gzip, hashlib, json, shutil, subprocess

w = Path(__file__).resolve().parent
s = Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
o = s.with_name('lux-engine')
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=s, text=True).strip()
base = '22ab1a30862f6bff943cef86d4232839173c7107'
f = w / 'final' / sha
build = s.parent / 'build/RelWithDebInfo' / ('p11-' + sha[:12])
prefix = s.parent / 'install' / ('P11-' + sha[:12])
a = s / 'dev_log/P11'
assert not a.exists(), 'Never overwrite a frozen archive'
assert not subprocess.check_output(['git', 'status', '--porcelain'], cwd=s, text=True).strip()
baseline = json.loads((w / 'baseline.json').read_text())
assert subprocess.check_output(['git', 'rev-parse', 'main'], cwd=o, text=True).strip() == baseline['main']
assert hashlib.sha256((o / 'editor/project/src/ProjectBuilder.cpp').read_bytes()).hexdigest() == baseline['user_file_sha256']
assert not subprocess.check_output(['git', 'diff', '--name-only', base, sha, '--', 'dev_log'], cwd=s).strip()

def write(path, data):
    path = a / path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

def copy(src, dest):
    target = a / dest
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, target)

def tree(src, dest):
    for path in sorted(src.rglob('*')):
        if path.is_file():
            copy(path, Path(dest) / path.relative_to(src))

a.mkdir(parents=True)
(a / '.gitattributes').write_bytes(b'* -text\n')

records = json.loads((f / 'commands.json').read_text())
by_name = {r['name']: r for r in records}
for name in ['ctest', 'sdk-all', 'cpu-ctest', 'player-ctest', 'clang-public-headers', 'regenerate-no-work']:
    assert by_name[name]['exit_code'] == 0, name
for r in records:
    assert r['implementation_sha'] == sha
    assert hashlib.sha256((f / r['log']).read_bytes()).hexdigest() == r['sha256']
    copy(f / r['log'], 'logs/' + r['log'])
    r['log'] = 'logs/' + r['log']
for path in f.glob('*'):
    if path.is_file() and path.name != 'commands.json':
        copy(path, 'logs/' + path.name)

for folder, name_prefix in [('sdk', ''), ('abi-independence', 'abi-'), ('public-headers', 'public-')]:
    tree(f / folder, 'evidence/' + ('abi' if folder == 'abi-independence' else folder))
    for r in json.loads((f / folder / 'commands.json').read_text()):
        r['name'] = name_prefix + r['name']
        r['log'] = 'evidence/' + ('abi' if folder == 'abi-independence' else folder) + '/' + r['log']
        r['implementation_sha'] = sha
        records.append(r)
assert len({r['name'] for r in records}) == len(records)
consumer_groups = sorted({r['name'].removesuffix('-configure') for r in records
                          if r['log'].startswith('evidence/sdk/') and r['name'].endswith('-configure')})
assert len(consumer_groups) == 24, consumer_groups
for group in consumer_groups:
    for step in ['configure', 'build', 'no-work', 'ctest']:
        assert next(r for r in records if r['name'] == group + '-' + step)['exit_code'] == 0

for name in ['baseline.json', 'source-map.json', 'behavior-map.json', 'command-map.json', 'owner-map.json',
             'regression-source-map.json', 'tests-before.json', 'files.json', 'source-audit.json',
             'installation-audit.json', 'retained-file-inventory.json']:
    copy(w / name, name)
tree(w / 'input', 'input')
tree(w / 'protected', 'protected')
tree(w / 'before', 'failures/before')
tree(w / 'runs', 'failures/development-runs')
for prior in sorted((w / 'final').iterdir()):
    if prior.is_dir() and prior.name != sha:
        # Preserve actual failed/intermediate commands without duplicating gigabytes of link graphs.
        for file in prior.glob('*'):
            if file.is_file() and file.name != 'actual-link-commands.log':
                copy(file, 'failures/nonfinal-' + prior.name + '/' + file.name)
        for name in ['sdk-new', 'sdk', 'public-headers', 'abi-independence']:
            if (prior / name).exists():
                tree(prior / name, 'failures/nonfinal-' + prior.name + '/' + name)
for script in w.glob('*.py'):
    copy(script, 'scripts/' + script.name)
copy(w / 'verify_archive.py', 'verify.py')

copy(build / 'editor-architecture/targets.json', 'dependency-map.json')
copy(build / 'CMakeCache.txt', 'evidence/CMakeCache.txt')
copy(build / 'install_manifest.txt', 'evidence/install_manifest.txt')
copy(s / 'editor/tests/architecture/rules.json', 'evidence/rules.json')
for src, target in [(build / 'compile_commands.json', 'evidence/compile_commands.json.gz')]:
    target = a / target
    with gzip.GzipFile(filename=str(target), mode='wb', mtime=0) as stream:
        stream.write(src.read_bytes())
tree(build / '.cmake/api/v1/reply', 'evidence/cmake-file-api')
copy(w.parent / 'migration-ledger.json', 'ledger-snapshot.json')
source_map = json.loads((w / 'source-map.json').read_text())
write('removal-plan.json', {'removed': [x for x in source_map if x['action'] == 'MOVED_ORIGINAL_DELETED'],
                          'p12_last_consumers': [x for x in source_map if x['action'] == 'RETAINED_PRODUCT_OR_TEST']})

receipt = dict(
    phase='P11', migration_stage='P11', layering_mode='STRICT', status='PASS',
    scope_revision='2026-10-01-closeout-1', branch='codex/p11-closeout', push_branch='codex/editor-redesign-v4',
    evidence_commit_recorded_outside_this_file=True,
    frozen_at=datetime.now(timezone.utc).isoformat(), input_sha=base, implementation_sha=sha,
    stop_after='P11', continuation_authorized=False, commands=records, consumer_groups=consumer_groups,
    worktrees=dict(implementation=str(s), original=str(o), original_head=baseline['original_head'],
                   main=baseline['main'], user_patch_applied=False, user_file_sha256=baseline['user_file_sha256']),
    platform_scope=dict(windows='PASS: actual MSVC/clang-cl, native CPU, PLAYER, SDK, IO, plugins, GPU/input',
                        linux='NOT_RUN', system_ime='NOT_RUN', sanitizer='NOT_RUN', old_50k='Original PARTIAL unchanged; not rerun'),
    defects=dict(C01=dict(current_result='FAIL', responsibility='P12 complete layout application'),
                 C03=dict(current_result='PASS', historical_result='FAIL retained in original snapshots',
                          evidence='Original old-product probe plus formal command reentrancy and actual SDK lifetime tests'),
                 C04=dict(current_result='FAIL', responsibility='P12 original startup/menu connection contract')),
    executable='Installed lux_editor remains the old product in P11. P12 switch not authorized.',
    qualification=dict(windows_editor='PASS', cpu='PASS', player='PASS', sdk='PASS', actual_editor_plugin='PASS',
                       actual_gpu='PASS', native_input='PASS', dependency_negatives='PASS', public_headers='PASS',
                       generated_rebuild='PASS', archive_checks='See archive-probes/results.json',
                       installed_product_start='P12_PRODUCT_CUTOVER'),
    protected_history='All previous dev_log trees byte-identical to input SHA.',
    actual_sdk_lifetime_failure='Development K actual installed DLL crash retained, with debugger stack; receiving-module owner fix qualified with and without extra configuration pin.',
    note='Implementation and evidence commits are separate. Test numbers supplement, never replace, behavior and source mappings.'
)
write('receipt.json', receipt)
copy(w / 'report.md', 'README.md')

def artifacts():
    return [{'path': str(p.relative_to(a)).replace('\\', '/'), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in sorted(a.rglob('*')) if p.is_file() and p.name != 'artifacts.json']

write('artifacts.json', artifacts())
print('Frozen', sha, 'commands', len(records), 'artifacts', len(artifacts()), 'groups', len(consumer_groups))
