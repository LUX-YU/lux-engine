from pathlib import Path
import hashlib, json, shutil, subprocess

w = Path(__file__).resolve().parent
r = Path('E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=r, text=True).strip()
archive = w / 'archive'
receipt = json.loads((archive / 'receipt.json').read_text())
assert receipt['implementation_sha'] == sha
probes = json.loads((w / 'archive-probes/results.json').read_text())
assert len(probes) == 5
assert all((p['exit_code'] == 0) == (p['expected'] == 'PASS') for p in probes)
commands = {c['name']: c for c in receipt['commands']}
for name in ['cpu-ctest', 'player-ctest', 'sdk-all', 'clang-public-headers', 'regenerate-no-work']:
    assert commands[name]['exit_code'] == 0
headers = json.loads((archive / 'evidence/public-headers/commands.json').read_text())
assert len(headers) == 115 and all(c['exit_code'] == 0 for c in headers)
ledger_path = w / 'ledger.json'
ledger = json.loads(ledger_path.read_text())
ledger.update(status='PASS', batch='L6_COMPLETE', next='WAIT_FOR_P10Q_REVIEW; P11 not authorized',
              file_plan='audit/file-plan.json', final_evidence='dev_log/P10Q-structure/receipt.json',
              stop_after='P10Q', continuation_authorized=False)
ledger['verification'].append({
    'batch': 'L6 final complete', 'status': 'PASS', 'implementation_sha': sha,
    'evidence': ['final/' + sha + '/commands.json', 'final/' + sha + '/sdk/commands.json',
                 'final/' + sha + '/public-headers/commands.json', 'archive-probes/results.json',
                 'audit/protected-final.json'],
    'notes': 'Editor 209/209, CPU 183/183, PLAYER 12/12; 21 installed groups; 115 standalone clang-cl public headers; actual GPU/native input; real dependency and compilation negatives; exact generation/no-work. Portable archive positive, missing and tampered real log rejection, restored and canonical positives. Original scope amendments and old failures unchanged.'})
ledger['archive_verification'] = {'status': 'PASS', 'evidence': 'archive-probes/results.json',
    'source': 'fixed Git implementation objects', 'evidence_paths': 'relative to portable archive'}
ledger_path.write_text(json.dumps(ledger, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
shutil.copyfile(ledger_path, archive / 'construction/ledger.json')
shutil.copyfile(Path(__file__), archive / 'construction/seal.py')
shutil.copyfile(w / 'audit/protected-final.json', archive / 'evidence/audit/protected-final.json')
manifest = [{'path': p.relative_to(archive).as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in sorted(archive.rglob('*')) if p.is_file() and p.name not in ['artifacts.json', 'receipt.json']]
(archive / 'artifacts.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
print('Sole mutable ledger and frozen snapshot sealed at', sha)
