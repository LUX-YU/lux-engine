from pathlib import Path
from datetime import datetime, timezone
import argparse, hashlib, json, os, subprocess, sys
if os.name == "nt":
    import ctypes
    ctypes.windll.kernel32.SetErrorMode(0x8003)

p = argparse.ArgumentParser()
p.add_argument("--cwd", default="E:/SyncForder/CodeRepos/lux-engine")
p.add_argument("--runtime", default="E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine-framework/bin")
p.add_argument("name")
p.add_argument("command", nargs=argparse.REMAINDER)
a = p.parse_args()
root = Path(__file__).resolve().parent
vs = "D:/Development/Mircosoft/VisualStudio/Common7/Tools/VsDevCmd.bat"
environment = subprocess.check_output(f'cmd /d /s /c ""{vs}" -arch=x64 -host_arch=x64 >nul && set"', text=True)
for line in environment.splitlines():
    if "=" in line:
        key, value = line.split("=", 1)
        os.environ[key] = value
os.environ["PATH"] = os.pathsep.join([
    a.runtime, "D:/Development/vcpkg/installed/x64-windows/bin",
    *[v for v in os.environ["PATH"].split(os.pathsep)
      if "/coderepos/install/" not in v.replace("\\", "/").lower()
      and "/coderepos/build/" not in v.replace("\\", "/").lower()]
])
log = root / "logs" / (a.name + ".log")
log.parent.mkdir(exist_ok=True)
if log.exists():
    raise SystemExit("Evidence already exists: " + str(log))
started = datetime.now(timezone.utc).isoformat()
def git(*args):
    r = subprocess.run(["git", *args], cwd=a.cwd, capture_output=True)
    return r.stdout
source_sha = git("rev-parse", "HEAD").decode().strip()
source_status = git("status", "--porcelain", "--untracked-files=all").decode(errors="replace")
source_diff_hash = hashlib.sha256(git("diff", "--binary", "HEAD")).hexdigest()
with log.open("wb") as output:
    result = subprocess.run(a.command, cwd=a.cwd, stdout=output, stderr=subprocess.STDOUT)
record = dict(implementation_sha=source_sha, worktree_status=source_status, diff_sha256=source_diff_hash, name=a.name, command=a.command, cwd=a.cwd, started=started,
              ended=datetime.now(timezone.utc).isoformat(), exit_code=result.returncode,
              log=log.relative_to(root).as_posix(),
              sha256=hashlib.sha256(log.read_bytes()).hexdigest())
records = root / "commands.json"
items = json.loads(records.read_text()) if records.exists() else []
items.append(record)
records.write_text(json.dumps(items, indent=2) + "\n")
print(a.name, result.returncode, flush=True)
print(log.read_text(errors="replace")[-6500:])
sys.exit(result.returncode)
