"""Finish the uncommitted P10Q snapshot after the actual archive probes succeed."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
work = repo / '.internal/editor-redesign'
archive = repo / 'dev_log/P10Q'
records = json.loads((archive / 'archive-probes/commands.json').read_text())
assert len(records) == 5
assert all(item['exit_code'] == item['expected_exit'] for item in records)
proofs = ['archive-probes/' + item['log'] for item in records]
coverage = json.loads((work / 'P10Q-coverage.json').read_text(encoding='utf-8'))
entry = next(item for item in coverage if item['id'] == 'XQ34')
entry['evidence'] = list(dict.fromkeys(entry['evidence'] + proofs))
entry['reason'] += '; real relocated archive accepted, missing/tampered CTest evidence rejected, restored archive accepted'
(work / 'P10Q-coverage.json').write_text(json.dumps(coverage, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

note = ('\n最终归档实际验证：完整材料复制到含中文和空格的新路径后通过；删除真实 CTest 日志、'
        '篡改该日志分别被拒绝；恢复原字节后通过。命令和输出见 archive-probes/，'
        '正式原路径复核通过，状态仍为 PARTIAL。此项证明证据可迁移，不改变 Linux、性能或 IME 状态。\n')
for name in ['P10Q-report.md', 'P10Q-work.md']:
    path = work / name
    path.write_text(path.read_text(encoding='utf-8') + note, encoding='utf-8')

ledger_path = work / 'migration-ledger.json'
ledger = json.loads(ledger_path.read_text(encoding='utf-8'))
ledger['p10q']['archive_verification'] = {
    'status': 'PASS',
    'implementation_sha': records[0]['implementation_sha'],
    'evidence': proofs,
    'scope': 'Actual relocated archive, missing/tampered real evidence, restored and canonical archive; overall qualification remains PARTIAL'
}
ledger_path.write_text(json.dumps(ledger, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
for source, target in [('P10Q-report.md', 'README.md'), ('migration-ledger.json', 'migration-ledger.json'),
                       ('P10Q-report.md', 'development/P10Q-report.md'), ('P10Q-work.md', 'development/P10Q-work.md'),
                       ('P10Q-coverage.json', 'development/P10Q-coverage.json')]:
    shutil.copyfile(work / source, archive / target)
shutil.copyfile(__file__, archive / 'tools/finalize_p10q_archive.py')

receipt_path = archive / 'receipt.json'
receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
receipt['coverage'] = coverage
receipt['archive_verification'] = ledger['p10q']['archive_verification']
receipt_path.write_text(json.dumps(receipt, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
artifacts = [{'path':path.relative_to(archive).as_posix(), 'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
             for path in sorted(archive.rglob('*')) if path.is_file() and path.name not in ['receipt.json','artifacts.json']]
(archive / 'artifacts.json').write_text(json.dumps(artifacts, indent=2) + '\n', encoding='utf-8')
subprocess.run([sys.executable, str(repo / 'editor/tests/architecture/validate_quality_evidence.py'),
                '--source', str(repo), '--archive', str(archive)], cwd=repo, check=True)
