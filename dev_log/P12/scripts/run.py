from pathlib import Path
import argparse, hashlib, json, os, subprocess, sys, datetime, time

work=Path(__file__).resolve().parent
repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p12')
build=repo.parent/'build/RelWithDebInfo/p12-dev'
prefix=repo.parent/'install/P11-1afb8f3f6e58'
parser=argparse.ArgumentParser();parser.add_argument('name');parser.add_argument('command',nargs=argparse.REMAINDER)
args=parser.parse_args()
vs=r'D:\Development\Mircosoft\VisualStudio\Common7\Tools\VsDevCmd.bat'
envtext=subprocess.check_output(f'cmd /d /s /c ""{vs}" -arch=x64 -host_arch=x64 >nul && set"',text=True)
for line in envtext.splitlines():
 if '=' in line:
  k,v=line.split('=',1);os.environ[k]=v
os.environ['PATH']=str(build/'bin')+os.pathsep+str(prefix/'bin')+os.pathsep+r'D:\Development\vcpkg\installed\x64-windows\bin'+os.pathsep+os.environ['PATH']
command=args.command
if not command:
 if args.name.endswith('configure'):
  baseline=json.loads((work.parent/'layering/final/e72e9f7f931c5825dcd6c2c5a5b40dd66ccf62a5/commands.json').read_text())
  command=next(c['argv'] for c in baseline if c['name']=='configure').copy()
  command[command.index('-S')+1]=str(repo);command[command.index('-B')+1]=str(build)
  command.extend(['-DLUX_EDITOR_LAYERING_MODE=STRICT', '-DLUX_EDITOR_MIGRATION_STAGE=P11', '-DCMAKE_PREFIX_PATH='+str(prefix), '-DCMAKE_INSTALL_PREFIX='+str(repo.parent/'install/P12-dev')])
  q=build/'.cmake/api/v1/query';q.mkdir(parents=True,exist_ok=True)
  for file in ['codemodel-v2','cache-v2','toolchains-v1']:(q/file).touch()
 elif args.name.endswith(('build','no-work')):command=['cmake','--build',str(build),'--target','all','-j','4','--','-k','0']
 else:raise RuntimeError('explicit command required')
folder=work/'runs';folder.mkdir(exist_ok=True)
log=folder/(args.name+'.log');i=1
while log.exists():log=folder/(args.name+f'-{i}.log');i+=1
print('Running',command,flush=True)
def fingerprint():
 h=hashlib.sha256(subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo))
 h.update(subprocess.check_output(['git','diff','--binary','HEAD'],cwd=repo,stderr=subprocess.DEVNULL))
 paths=subprocess.check_output(['git','ls-files','-o','--exclude-standard','-z'],cwd=repo).decode().split('\0')
 for path in sorted(filter(None,paths)):
  file=repo/path
  h.update(path.encode());h.update(b'\0');h.update(file.read_bytes() if file.is_file() else b'<deleted>')
 return h.hexdigest()
source_before=fingerprint()
started=datetime.datetime.now(datetime.timezone.utc).isoformat(); clock=time.monotonic()
with log.open('wb') as output: result=subprocess.run(command,cwd=repo,stdout=output,stderr=subprocess.STDOUT)
record={'started_utc':started,'elapsed_seconds':time.monotonic()-clock,'name':args.name,'argv':command,'cwd':str(repo),'exit_code':result.returncode,
        'source_before':source_before, 'source_after':fingerprint(), 'log':log.relative_to(work).as_posix(),'sha256':hashlib.sha256(log.read_bytes()).hexdigest(),
        'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
        'diff_sha256':hashlib.sha256(subprocess.check_output(['git','diff','HEAD'],cwd=repo,stderr=subprocess.DEVNULL)).hexdigest()}
records=folder/'commands.json';data=json.loads(records.read_text()) if records.exists() else []
data.append(record);records.write_text(json.dumps(data,indent=2))
print('Exit',result.returncode,'log',log,flush=True)
print(log.read_text(errors='replace')[-8000:] if result.returncode else log.read_text(errors='replace')[-1500:])
sys.exit(result.returncode)
