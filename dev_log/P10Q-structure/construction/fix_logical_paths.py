from pathlib import Path
import json,subprocess

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
work=Path(__file__).parent
plan=json.loads((work/'file-plan.json').read_text())
mapping={}
for x in plan:
 if '/include/' not in x['source'] or '/include/' not in x['destination']:continue
 old=x['destination'];root,logical=old.split('/include/',1)
 correct=x['source'].split('/include/',1)[1]
 if logical==correct:continue
 new=root+'/include/'+correct
 assert (repo/old).exists() and not (repo/new).exists()
 (repo/new).parent.mkdir(parents=True,exist_ok=True)
 (repo/old).rename(repo/new)
 mapping[old]=new
 x['destination']=new
for root in ['editor','cmake']:
 for p in (repo/root).rglob('*'):
  if not p.is_file() or p.suffix not in ('.txt','.json','.cmake','.py'):continue
  s=p.read_text(encoding='utf-8-sig');new=s
  for a,b in mapping.items():new=new.replace(a,b)
  if p.name=='CMakeLists.txt' and p.parent==repo/'editor/activities/scene':
   new=new.replace('include/lux/engine/editor/activities/scene','include/lux/engine/editor/editing/scene')
  if s!=new:p.write_text(new,newline='\n')
(work/'file-plan.json').write_text(json.dumps(plan,ensure_ascii=False,indent=2)+'\n')
p=work/'header-owners.json';owners=json.loads(p.read_text());owners={mapping.get(k,k):v for k,v in owners.items()}
# This shared status is a pure value, not the old EditHistoryTarget/AssetSave adapter.
owners['editor/editing/include/lux/engine/editor/CloseStatus.hpp']='editor_contracts'
p.write_text(json.dumps(owners,indent=2)+'\n')
p=repo/'editor/editing/CMakeLists.txt';s=p.read_text()
s=s.replace('    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/CloseStatus.hpp\n','')
needle='    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/EditorError.hpp\n'
s=s.replace(needle,needle+'    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/CloseStatus.hpp\n')
p.write_text(s,newline='\n')
p=repo/'editor/tests/architecture/rules.json';r=json.loads(p.read_text())
r['foundation_targets']['editor_contracts']['files'].append('editor/editing/include/lux/engine/editor/CloseStatus.hpp')
r['foundation_targets']['editor_contracts']['public_headers'].append('lux/engine/editor/CloseStatus.hpp')
p.write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
(work/'logical-path-correction.json').write_text(json.dumps({'corrected':mapping,'cause':'Physical root substitution in initial file-plan also changed logical include segments. Developer prefix masked it; final clean SDK is mandatory.'},indent=2)+'\n')
print(mapping)
