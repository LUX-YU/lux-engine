"""Launch the sole installed product, observe its own native window/modules, request normal close."""
from pathlib import Path
import argparse, ctypes, json, shutil, subprocess, time, os, hashlib, tomllib
from ctypes import wintypes
p=argparse.ArgumentParser();p.add_argument('--prefix',type=Path,required=True);p.add_argument('--build',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--manual',action='store_true');a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
fixture='Initial3D' if a.manual else 'MinimalProject'
source=max((a.build/'editor/tests/integration/application').glob('*/'+fixture+'/Project.luxproject'), key=lambda p:p.stat().st_mtime_ns)
project=a.output/'正式产品 空格路径';assert not project.exists();shutil.copytree(source.parent,project)
def project_files():
 return [{'path':f.relative_to(project).as_posix(),'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in sorted(project.rglob('*')) if f.is_file()]
(a.output/'project-before.json').write_text(json.dumps(project_files(),ensure_ascii=False,indent=2),encoding='utf8')
exe=a.prefix/'bin/lux_editor.exe';assert exe.is_file()
os.environ['PATH']=str(a.prefix/'bin')+os.pathsep+'D:/Development/vcpkg/installed/x64-windows/bin'+os.pathsep+os.pathsep.join(v for v in os.environ['PATH'].split(os.pathsep) if '/install/' not in v.replace('\\','/').lower() and '/coderepos/build/' not in v.replace('\\','/').lower())
user=ctypes.WinDLL('user32',use_last_error=True);kernel=ctypes.WinDLL('kernel32',use_last_error=True);psapi=ctypes.WinDLL('psapi',use_last_error=True)
user.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
user.IsWindowVisible.argtypes=[wintypes.HWND];user.PostMessageW.argtypes=[wintypes.HWND,wintypes.UINT,wintypes.WPARAM,wintypes.LPARAM]
callback=ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM);user.EnumWindows.argtypes=[callback,wintypes.LPARAM]
args=[str(exe),'--project',str(project/'Project.luxproject')]
recent=Path(os.environ['LOCALAPPDATA'])/'lux/editor/recent-projects.toml'
prior=recent.read_bytes() if recent.exists() else None
# Do not change the user's global recent-project preferences as a testing side effect.
try:
 with (a.output/'product-output.log').open('wb') as f:
  process=subprocess.Popen(args,cwd=a.prefix/'bin',stdout=f,stderr=subprocess.STDOUT)
  deadline=time.monotonic()+30;windows=[]
  @callback
  def visit(hwnd,_):
   pid=wintypes.DWORD();user.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
   if pid.value==process.pid and user.IsWindowVisible(hwnd):windows.append(hwnd)
   return True
  while not windows and process.poll() is None and time.monotonic()<deadline:
   user.EnumWindows(visit,0);time.sleep(.02)
  assert windows,('No product window',process.poll())
  # Keep the normal product frame loop alive; no alternate test executable or exit branch.
  time.sleep(3)
  assert process.poll() is None
  modules=(wintypes.HMODULE*2048)();needed=wintypes.DWORD();handle=wintypes.HANDLE(int(process._handle))
  psapi.EnumProcessModules.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.HMODULE),wintypes.DWORD,ctypes.POINTER(wintypes.DWORD)]
  psapi.GetModuleFileNameExW.argtypes=[wintypes.HANDLE,wintypes.HMODULE,wintypes.LPWSTR,wintypes.DWORD]
  assert psapi.EnumProcessModules(handle,modules,ctypes.sizeof(modules),ctypes.byref(needed))
  paths=[]
  for module in modules[:needed.value//ctypes.sizeof(wintypes.HMODULE)]:
   name=ctypes.create_unicode_buffer(32768);assert psapi.GetModuleFileNameExW(handle,module,name,len(name));paths.append(name.value)
  (a.output/'loaded-modules.json').write_text(json.dumps(paths,indent=2))
  assert all('/coderepos/build/' not in x.replace('\\','/').lower() for x in paths)
  assert all('/install/' not in x.replace('\\','/').lower() or str(a.prefix).replace('\\','/').lower() in x.replace('\\','/').lower() for x in paths)
  if not a.manual: assert user.PostMessageW(windows[0],0x10,0,0)
  print('Installed product ready for actual menu validation' if a.manual else 'Normal close requested',flush=True)
  result=process.wait(timeout=1800 if a.manual else 40)
  assert result==0,result
  (a.output/'project-after.json').write_text(json.dumps(project_files(),ensure_ascii=False,indent=2),encoding='utf8')
  print('PASS sole installed lux_editor: formal bootstrap, UTF-8 project path, native window, normal close, no build/old SDK modules',flush=True)

finally:
 current=recent.read_bytes() if recent.exists() else None
 if current!=prior:
  rows=tomllib.loads(current.decode('utf8'))
  previous=tomllib.loads(prior.decode('utf8'))['projects'] if prior is not None else []
  expected=[(project/'Project.luxproject').as_posix()]
  for entry in previous:
   value=Path(entry).as_posix()
   if value not in expected and len(expected)<20: expected.append(value)
  assert rows=={'version':1,'projects':expected}, 'Concurrent preference change; preserve actual file without overwriting'
  if prior is None: recent.unlink()
  else: recent.write_bytes(prior)
 assert (recent.read_bytes() if recent.exists() else None)==prior
 (a.output/'preference-preservation.json').write_text(json.dumps({'preserved':True,'original_sha256':hashlib.sha256(prior).hexdigest() if prior is not None else None},indent=2))
