from pathlib import Path
import json,subprocess,re
w=Path(__file__).resolve().parent;r=Path('E:/SyncForder/CodeRepos/lux-engine-p10q-structure');sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip();rules=json.loads((r/'editor/tests/architecture/rules.json').read_text())['editor_layering'];plan=json.loads((w/'file-plan.json').read_text());files=set(subprocess.check_output(['git','ls-files'],cwd=r,text=True).splitlines());cpp={p:(r/p).read_text(encoding='utf-8-sig',errors='replace') for p in files if p.endswith(('.cpp','.hpp','.h','.cc'))};out=w/'audit';out.mkdir(exist_ok=True)
for x in plan:
 dst=x['destination'];assert dst in files,(x['source'],dst)
 x['final_action']=x['action'];x['final_blob']=subprocess.check_output(['git','rev-parse',sha+':'+dst],cwd=r,text=True).strip();x['final_providers']=rules['files'].get(dst,[])
 x['final_evidence']='files.json; target-map.json; final build/compiler/SDK logs'
 x['final_consumers']=[p for p,s in cpp.items() if x.get('logical_include') and re.search(r'#\s*include\s*[<"]'+re.escape(x['logical_include'])+r'[>"]',s)]
 if x['source']!=dst and x['source'] in files:
  assert x['source'] in ['editor/assets/CMakeLists.txt','editor/assets/README.md']
  x['retained_responsibility']='Only legacy AssetSource/AssetSave adapter target, registered product consumers until P12. AssetImporter original body removed.'
(out/'file-plan.json').write_text(json.dumps(plan,indent=2,ensure_ascii=False)+'\n')
retained={n:v for n,v in rules['targets'].items() if v['layer']=='RETAINED'};(out/'retained-product.json').write_text(json.dumps(retained,indent=2)+'\n')
print('Verified',len(plan),'original tracked Editor files;',len(retained),'retained target declarations')
base='f7c27f9375cbf8dd8af37b30a6027a460de26213';evidence=[]
for x in plan:
 p=x['source'];dst=x['destination']
 if ('/test/' in p or '/tests/' in p) and p.endswith(('.cpp','.hpp')):
  old=subprocess.check_output(['git','show',base+':'+p],cwd=r).decode();new=(r/dst).read_text();norm=lambda s:re.sub(r'\s+','',re.sub(r'^\s*#\s*include[^\n]*','',s,flags=re.M))
  evidence.append({'old':p,'new':dst,'old_blob':x['blob'],'new_blob':x['final_blob'],'identical_except_includes_and_whitespace':norm(old)==norm(new)})
assert all(x['identical_except_includes_and_whitespace'] for x in evidence),[x for x in evidence if not x['identical_except_includes_and_whitespace']]
(out/'assertions-preserved.json').write_text(json.dumps(evidence,indent=2)+'\n')
print('Original',len(evidence),'Editor C++ test/support bodies exactly preserve non-include tokens')
