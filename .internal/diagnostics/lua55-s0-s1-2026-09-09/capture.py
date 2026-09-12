"""Read-only identity capture and bounded preservation before reusing qualification slots."""
import datetime, hashlib, json, shutil, subprocess, sys, zipfile
from pathlib import Path

root = Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=True)
source = Path(__file__).resolve().parents[3]
repos = Path('E:/SyncForder/CodeRepos')

def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).rstrip('\r\n')

def identity(path):
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.file_digest(path.open('rb'), 'sha256').hexdigest())

result = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), repositories=[], builds=[])
for path in [repos/'lux-engine', source, repos/'lux-cxx', repos/'lux-cmake-toolset',
             repos/'build/RelWithDebInfo/script-region-opt/final-source']:
    status = git(path, 'status', '--porcelain=v1', '-uall')
    files = []
    for line in status.splitlines():
        item = path / line[3:]
        if item.is_file(): files.append(identity(item))
    result['repositories'].append(dict(path=str(path), head=git(path, 'rev-parse', 'HEAD'),
        status=status, files=files))

for slot in ['d', 'l', 't']:
    build = repos/'build/RelWithDebInfo/o/w'/slot
    saved = root/'original-images'/slot
    saved.mkdir(parents=True, exist_ok=True)
    files = []
    # Preserve runnable benchmark dependency closure, not a second full build tree.
    for path in sorted((build/'bin').glob('*')):
        keep = path.suffix.lower() in ['.dll', '.pdb'] or path.name == 'script_runtime_benchmark.exe'
        if path.is_file() and keep:
            target = saved/path.name
            if target.exists(): raise RuntimeError(f'Refusing overwrite: {target}')
            shutil.copy2(path, target)
            files.append(identity(path))
    for name in ['CMakeCache.txt', 'compile_commands.json']:
        shutil.copy2(build/name, saved/name)
    result['builds'].append(dict(path=str(build), files=files))

bundle = Path('C:/Users/ChenHui/Downloads/LUX_Script_Optimization_Implementation_Plan_2026-09-09_v1.zip')
result['attachment'] = identity(bundle)
assert result['attachment']['sha256'] == 'c79081b1c9b32e31f5b26b7c19155a4c11073dcf9e62e7501f76a28e3eeba003'
with zipfile.ZipFile(bundle) as archive:
    target = root/'instructions'
    for item in archive.infolist():
        path = (target/item.filename).resolve()
        if not path.is_relative_to(target): raise RuntimeError(item.filename)
        if item.is_dir(): continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(archive.read(item))
result['space'] = dict(zip(['total', 'used', 'free'], shutil.disk_usage(root)))
(root/'baseline-start.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(dict(identity=str(root/'baseline-start.json'), space=result['space'])))
