import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

commit = 'c01ca39c5333590bb928efa5ee8a1bed28ed8fcd'
transport = Path('E:/SyncForder/CodeRepos/lux-engine-sr3-remote-check')
destination = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/sr4-remote-c01ca39c')
destination.mkdir(exist_ok=False)
report = Path('E:/SyncForder/CodeRepos/lux-engine-deep-optimization/.internal/evidence/script/sr4/remote-verification.json')
def git(*args):
    return subprocess.check_output(['git', '-C', str(transport), *args])
before_status = hashlib.sha256(git('status', '--porcelain')).hexdigest()
fetch = subprocess.run(['git', '-C', str(transport), 'fetch', 'origin', commit], capture_output=True)
(destination / 'fetch.log').write_bytes(fetch.stdout + fetch.stderr)
fetch.check_returncode()
base = '.internal/evidence/script/sr4/final/'
for name in ('SR4-raw-evidence.zip', 'SHA256SUMS', 'raw-files.json'):
    with (destination / name).open('wb') as stream:
        subprocess.run(['git', '-C', str(transport), 'show', commit + ':' + base + name], stdout=stream, check=True)
archive = destination / 'SR4-raw-evidence.zip'
actual = hashlib.sha256(archive.read_bytes()).hexdigest()
expected = (destination / 'SHA256SUMS').read_text().split()[0]
assert actual == expected
index = json.loads((destination / 'raw-files.json').read_text())
with zipfile.ZipFile(archive) as bundle:
    assert len(bundle.namelist()) == len(index) == 1743
    assert set(bundle.namelist()) == {entry['path'] for entry in index}
    for entry in index:
        assert Path(entry['path']).suffix.lower() not in {'.exe', '.dll', '.obj', '.pdb', '.lib'}
        content = bundle.read(entry['path'])
        assert len(content) == entry['size']
        assert hashlib.sha256(content).hexdigest() == entry['sha256'], entry['path']
after_status = hashlib.sha256(git('status', '--porcelain')).hexdigest()
assert before_status == after_status
result = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'origin': git('remote', 'get-url', 'origin').decode().strip(),
          'archive_commit': commit, 'repository_path': base + archive.name,
          'transport_repo': str(transport), 'download_directory': str(destination),
          'method': 'Fetch exact remote commit, then git show each blob into new files; validate ZIP against remotely read index',
          'archive_sha256': actual, 'archive_bytes': archive.stat().st_size,
          'indexed_files': len(index), 'verified_files': len(index), 'compiled_binaries': 0,
          'transport_status_sha256_before': before_status, 'transport_status_sha256_after': after_status,
          'status': 'PASS'}
report.write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
