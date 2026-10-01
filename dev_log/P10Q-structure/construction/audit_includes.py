from pathlib import Path
import json,re,subprocess
work=Path(__file__).resolve().parent
repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
plan=json.loads((work/'file-plan.json').read_text())
rows=[]
for x in plan:
 p=repo/x['destination']
 if not p.is_file():p=repo/x['source']
 if not p.is_file() or p.suffix not in ('.cpp','.hpp','.h'):continue
 old=subprocess.check_output(['git','show','f7c27f9375cbf8dd8af37b30a6027a460de26213:'+x['source']],cwd=repo).decode('utf-8-sig')
 before=set(re.findall(r'#include <([^>]+)>',old))
 after=set(re.findall(r'#include <([^>]+)>',p.read_text()))
 if before!=after:rows.append({'file':p.relative_to(repo).as_posix(),'removed':sorted(before-after),'added':sorted(after-before)})
print(json.dumps(rows,indent=2))
(work/'include-migration-audit.json').write_text(json.dumps(rows,indent=2))
