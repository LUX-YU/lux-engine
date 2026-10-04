from pathlib import Path
import hashlib,json,re,subprocess,sys
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text());sdk=Path(c['prefix'])
roots=[sdk/'bin',Path('D:/Development/vcpkg/installed/x64-windows/bin')]
exe=Path(sys.argv[1]);seen={}
def visit(path):
 key=str(path).lower()
 if key in seen:return set()
 text=subprocess.check_output(['dumpbin','/dependents',str(path)],text=True,errors='replace')
 names=re.findall(r'^\s+(\S+\.dll)\s*$',text,re.M|re.I)
 seen[key]={'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'imports':names}
 blocked=set()
 for name in names:
  if name.lower()=='lux_engine_scene_composition.dll' and not (sdk/'bin'/name).is_file():
   blocked.add(name);continue
  target=next((root/name for root in roots if (root/name).is_file()),None)
  if target:blocked.update(visit(target))
 return blocked
blocked=visit(exe)
(w/'resumed-project-consumer-imports.json').write_text(json.dumps({'executable':str(exe),'blocked':sorted(blocked),'files':list(seen.values()),'note':'Static PE closure only; does not qualify arbitrary dynamic plugin loads.'},indent=2)+'\n')
print('Blocked PE imports:',sorted(blocked));sys.exit(bool(blocked))
