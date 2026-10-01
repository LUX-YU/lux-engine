from pathlib import Path
import json, os, posixpath, re, subprocess

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
work=Path(__file__).resolve().parent
plan=json.loads((work/'file-plan.json').read_text())
locations={x['source']:x['destination'] if (repo/x['destination']).is_file() else x['source'] for x in plan}
rulesfile=repo/'editor/tests/architecture/rules.json';rules=json.loads(rulesfile.read_text())
for original,current in locations.items():
    file=repo/current
    if not file.is_file() or file.suffix not in ('.hpp','.cpp','.h'):continue
    old=subprocess.check_output(['git','show','f7c27f9375cbf8dd8af37b30a6027a460de26213:'+original],cwd=repo).decode('utf-8-sig')
    text=file.read_text();updated=text
    for header in re.findall(r'#include "([^"]+)"',old):
        if '..' not in header:continue
        target=posixpath.normpath(posixpath.join(posixpath.dirname(original),header))
        if target not in locations:continue
        actual=locations[target]
        new=os.path.relpath(repo/actual,file.parent).replace('\\','/')
        updated=re.sub(r'(#include ")[^"]*'+re.escape(posixpath.basename(header))+r'(")',r'\g<1>'+new+r'\2',updated)
        for policy in rules.values():
            if isinstance(policy,dict):
                for key,headers in policy.get('test_headers',{}).items():
                    if key==current:policy['test_headers'][key]=[new if h==header or h.endswith('/'+posixpath.basename(header)) else h for h in headers]
    if updated!=text:file.write_text(updated,newline='\n');print(current)
rulesfile.write_text(json.dumps(rules,ensure_ascii=False,indent=2)+'\n',newline='\n')
