from pathlib import Path
from datetime import datetime, timezone
import argparse, hashlib, json, os, subprocess, sys

p = argparse.ArgumentParser()
p.add_argument('--runtime', required=True)
p.add_argument('--cwd', default='E:/SyncForder/CodeRepos/lux-engine-ec2')
p.add_argument('name')
p.add_argument('command', nargs=argparse.REMAINDER)
a = p.parse_args()
w = Path(__file__).resolve().parent
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
vs = 'D:/Development/Mircosoft/VisualStudio/Common7/Tools/VsDevCmd.bat'
env = subprocess.check_output(f'cmd /d /s /c ""{vs}" -arch=x64 -host_arch=x64 >nul && set"', text=True)
for line in env.splitlines():
    if '=' in line:
        key, value = line.split('=', 1)
        os.environ[key] = value
os.environ['PATH'] = os.pathsep.join([a.runtime, 'D:/Development/vcpkg/installed/x64-windows/bin', *[
    entry for entry in os.environ['PATH'].split(os.pathsep)
    if '/coderepos/install/' not in entry.replace('\\', '/').lower()
    and '/coderepos/build/' not in entry.replace('\\', '/').lower()
]])
logs = w / 'logs'
logs.mkdir(exist_ok=True)
log = logs / (a.name + '.log')
assert not log.exists(), 'Do not overwrite evidence'
before = subprocess.check_output(['git', '-c', 'core.autocrlf=false', 'diff', '--binary', 'HEAD'], cwd=s)
untracked = subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=s).decode().split('\0')
untracked_hashes = {name: hashlib.sha256((s / name).read_bytes()).hexdigest() for name in untracked if name}
source_head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=s, text=True).strip()
started = datetime.now(timezone.utc).isoformat()
with log.open('wb') as output:
    result = subprocess.run(a.command, cwd=a.cwd, stdout=output, stderr=subprocess.STDOUT)
record = dict(name=a.name, argv=a.command, cwd=a.cwd, runtime_path=a.runtime,
              started=started, ended=datetime.now(timezone.utc).isoformat(), exit_code=result.returncode,
              source_head=source_head,
              source_head_after=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=s, text=True).strip(),
              source_diff_sha256=hashlib.sha256(before).hexdigest(),
              source_untracked_sha256=untracked_hashes,
              log=log.relative_to(w).as_posix(), sha256=hashlib.sha256(log.read_bytes()).hexdigest())
file = w / 'commands.json'
records = json.loads(file.read_text()) if file.exists() else []
records.append(record)
file.write_text(json.dumps(records, indent=2) + '\n')
print(a.name, result.returncode, log, flush=True)
print(log.read_text(errors='replace')[-5000:])
sys.exit(result.returncode)
