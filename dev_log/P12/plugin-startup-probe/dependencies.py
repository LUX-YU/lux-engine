import os,re,subprocess,json
from pathlib import Path
prefix=Path('E:/SyncForder/CodeRepos/install/P12-8d4aa55f840d/bin')
seen=set();missing={};pending=['lux_engine_physics2d_simulation.dll','render_features.dll','builtin_runtime_plugin.dll','lux_engine_scene_render.dll']
while pending:
 name=pending.pop()
 if name.lower() in seen:continue
 seen.add(name.lower());p=prefix/name
 if not p.exists():continue
 output=subprocess.check_output(['dumpbin','/dependents',str(p)],text=True)
 for dep in re.findall(r'^\s+(\S+\.dll)\s*$',output,re.M|re.I):
  if (prefix/dep).exists():pending.append(dep)
  elif not (Path(os.environ['SystemRoot'])/'System32'/dep).exists() and not dep.lower().startswith(('api-ms-','ext-ms-')):
   missing.setdefault(dep,[]).append(name)
print(json.dumps(missing,indent=2));print('Scanned',len(seen))
