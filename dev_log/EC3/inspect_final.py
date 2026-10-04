"""Inspect actual CMake providers and tracked removal paths, without inventing execution evidence."""
from pathlib import Path
import json, subprocess
w=Path(__file__).resolve().parent
c=json.loads((w/'final-config.json').read_text()); s=Path(c['source']); b=Path(c['build'])
reply=b/'.cmake/api/v1/reply'
index=json.loads(max(reply.glob('index-*.json'),key=lambda p:p.stat().st_mtime).read_text())
model=json.loads((reply/index['reply']['codemodel-v2']['jsonFile']).read_text())
targets={}
for item in model['configurations'][0]['targets']:
    value=json.loads((reply/item['jsonFile']).read_text()); targets[value['id']]=value
records=[]
for value in targets.values():
    if not value['paths']['source'].startswith(('editor','modules/platform/window','modules/function/ui')): continue
    records.append(dict(name=value['name'],type=value['type'],path=value['paths']['source'],
        artifacts=value.get('artifacts',[]),
        dependencies=[targets[d['id']]['name'] for d in value.get('dependencies',[]) if d['id'] in targets],
        sources=[x['path'] for x in value.get('sources',[])],
        compile_groups=value.get('compileGroups',[]),link=value.get('link')))
(w/'target-providers.json').write_text(json.dumps(dict(implementation_sha=c['implementation_sha'],targets=records),indent=2)+'\n')
tracked=subprocess.check_output(['git','ls-files'],cwd=s,text=True).splitlines()
removed=['editor/workbench/scene/codegen/inspector_codegen.py','editor/application/extensions/src/BuiltinContributions.cpp']
assert all(path not in tracked for path in removed)
result={}
for token in ['CameraPose','BuiltinContributions','prepareSave','rememberSave','settleSaves']:
    hits=[]
    for path in tracked:
        if not path.startswith(('editor/','engine/','modules/','cmake/')) or not path.endswith(('.hpp','.cpp','.cmake')):continue
        import re
        text=(s/path).read_text(encoding='utf-8-sig',errors='replace')
        for i,line in enumerate(text.splitlines(),1):
            if re.search(r'\b'+token+r'\b',line):hits.append(dict(path=path,line=i,text=line.strip()))
    result[token]=hits
assert not any(result.values()),result
(w/'removed-symbols.json').write_text(json.dumps(dict(implementation_sha=c['implementation_sha'],
    removed_paths=removed,production_scans=result,retained='test/inspector_codegen.py invokes installed host tool; not a C++ production emitter'),indent=2)+'\n')
p=w.parent/'migration-ledger.json'; j=json.loads(p.read_text()); j['ec3']['actual_target_evidence']='EC3/target-providers.json'
j['ec3']['removed_symbol_evidence']='EC3/removed-symbols.json'
p.write_text(json.dumps(j,ensure_ascii=False,indent=2)+'\n')
print(len(records),'configured providers; removed original paths and symbols confirmed')
