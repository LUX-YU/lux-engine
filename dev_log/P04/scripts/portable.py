"""Exercise the frozen P04 gate with only archived files and Git object access."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
work = root / '.internal/editor-redesign'
archive = root / 'dev_log/P04'
validation = archive / 'validation'
validation.mkdir(exist_ok=True)
git_dir = subprocess.check_output(['git', 'rev-parse', '--absolute-git-dir'], cwd=root, text=True).strip()
replica = Path(tempfile.mkdtemp(prefix='lux-p04-archive-')).resolve()
results = []


def run(name, expected, marker):
    result = subprocess.run([sys.executable, str(replica / 'dev_log/P04/check_receipt.py'),
                             '--evidence-root', str(replica)], cwd=replica,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = validation / (name + '.log')
    log.write_bytes(result.stdout)
    matches = result.returncode == expected and marker.encode() in result.stdout
    results.append({'name': name, 'exit_code': result.returncode, 'expected_exit_code': expected,
                    'marker': marker, 'matched': matches, 'archive_log': log.relative_to(root).as_posix()})
    print(name, result.returncode, matches, flush=True)
    assert matches, result.stdout.decode(errors='replace')


try:
    for phase in ['P00', 'P01', 'P01-R1', 'P02', 'P02-R1', 'P03', 'P03-R1', 'P04']:
        shutil.copytree(root / 'dev_log' / phase, replica / 'dev_log' / phase)
    (replica / '.git').write_text('gitdir: ' + git_dir + '\n', encoding='utf-8')
    assert not (replica / 'editor').exists() and not (replica / '.internal').exists()
    run('relocated', 0, 'PASS P04 gate')
    missing = replica / 'dev_log/P04/logs/flow-reading.log'
    contents = missing.read_bytes()
    missing.unlink()
    try:
        run('missing-required-log', 1, 'EVIDENCE_INCOMPLETE')
    finally:
        missing.write_bytes(contents)
    missing.write_bytes(contents + b'\ncorrupted evidence\n')
    try:
        run('altered-required-log', 1, 'sha256')
    finally:
        missing.write_bytes(contents)
    predecessor = replica / 'dev_log/P03-R1/receipt.json'
    contents = predecessor.read_bytes()
    predecessor.unlink()
    try:
        run('missing-predecessor', 1, 'EVIDENCE_INCOMPLETE')
    finally:
        predecessor.write_bytes(contents)
    run('restored', 0, 'PASS P04 gate')
finally:
    assert replica.is_relative_to(Path(tempfile.gettempdir()).resolve())
    assert replica.name.startswith('lux-p04-archive-')
    shutil.rmtree(replica)
    (validation / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')

shutil.copyfile(Path(__file__), archive / 'scripts/portable.py')
manifest = [{'archive_path': p.relative_to(root).as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in sorted(archive.rglob('*')) if p.is_file() and p.name != 'artifacts.json']
(archive / 'artifacts.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
final = subprocess.run([sys.executable, str(archive / 'check_receipt.py')], cwd=root,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(work / 'P04-final-gate.log').write_bytes(final.stdout)
print(final.stdout.decode(errors='replace'), end='')
raise SystemExit(final.returncode)
