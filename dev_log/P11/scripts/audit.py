from pathlib import Path
import subprocess,json,re,hashlib
w=Path(__file__).resolve().parent;s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=s,text=True).strip();base='22ab1a30862f6bff943cef86d4232839173c7107'
files=subprocess.check_output(['git','diff','--name-only',base,sha],cwd=s,text=True).splitlines()
records=[]
for path in files:
 r=subprocess.run(['git','show',sha+':'+path],cwd=s,capture_output=True)
 records.append({'path':path,'deleted':bool(r.returncode),'sha256':None if r.returncode else hashlib.sha256(r.stdout).hexdigest()})
(w/'files.json').write_text(json.dumps(records,indent=2)+'\n')
# Installation consumers and generators are scanned as actual source inputs, not inferred from filenames.
paths=subprocess.check_output(['git','ls-tree','-r','--name-only',sha],cwd=s,text=True).splitlines()
old=['lux/engine/editor/metadata/ConfigurationValue.hpp','lux/engine/editor/metadata/EditorReflection.hpp']
residual=[]
for path in paths:
 if path.startswith('dev_log/') or not path.endswith(('.cpp','.hpp','.cmake','.py','CMakeLists.txt')):continue
 text=(s/path).read_text(errors='replace')
 for header in old:
  if header in text:residual.append({'path':path,'header':header})
assert not residual,residual
formal=[]
for path in paths:
 if not path.startswith(('editor/editing/','editor/authoring/','editor/activities/','editor/workbench/','editor/application/')):continue
 if not path.endswith(('.cpp','.hpp')):continue
 text=(s/path).read_text(errors='replace')
 for header in re.findall(r'#\s*include\s*[<"]([^>"\n]+)',text):
  if any(x in header for x in ['EditorContext.hpp','SceneEditor.hpp','MaterialEditor.hpp','FlowForgeEditor.hpp','CommandRegistration.hpp','EditorPluginExports.hpp']):formal.append({'path':path,'header':header})
(w/'source-audit.json').write_text(json.dumps({'implementation_sha':sha,'moved_header_residuals':residual,'existing_private_product_inputs':formal,'formal_new_targets':'editor_commands/session_factories/editor_configuration/editor_extensions; exact graph in final target-map','old_product_executable':'lux_editor remains editor/app until P12','public_modules_modified':any(x.startswith('modules/') and '/include/' in x for x in files)},indent=2)+'\n')
print('Exact changed files',len(records),'deleted old includes absent; retained private inputs',len(formal))
build=s.parent/'build/RelWithDebInfo'/('p11-'+sha[:12]);prefix=s.parent/'install'/('P11-'+sha[:12])
graph=json.loads((build/'editor-architecture/targets.json').read_text());names={x['name'] for x in graph}
local_names={x['name'] for x in graph if x['IMPORTED']=='FALSE'}
rows=json.loads((w/'source-map.json').read_text())
for row in rows:
 if row['providers']==['scene_render_meta/render_feature_meta/physics2d_editor']:
  row['providers']=['scene_render_meta','render_feature_meta','physics2d_editor']
 row['providers']=[name for name in row['providers'] if name in local_names]
 assert row['providers'],row['path']
 assert set(row['providers'])<=names,(row['path'],row['providers'])
 row['target_consumers']=[t['name'] for t in graph if any(re.search(r'(?<!\w)'+re.escape(p)+r'(?!\w)',t.get('edges','')) for p in row['providers'])]
 row['status']='P11 implemented; retained rows remain solely P12 product consumers'
(w/'source-map.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
missing=[h for h in old if (prefix/'include'/h).exists()]
assert not missing,missing
assert not (prefix/'include/lux/engine/editor/plugins/ConfigurationForm.hpp').exists()
manifest=(build/'install_manifest.txt').read_text().splitlines()
assert all(Path(p).resolve().is_relative_to(prefix.resolve()) for p in manifest)
expected=['editor_commands','session_factories','editor_configuration','editor_extensions']
for name in expected:assert name in names,name
result={'implementation_sha':sha,'fresh_prefix':str(prefix),'old_installed_headers_absent':old,
 'new_providers':[x for x in graph if x['name'] in expected],
 'retained_sources':len(rows),'moved_originals':sum(x['action']=='MOVED_ORIGINAL_DELETED' for x in rows),
 'actual_install_entries':len(manifest),'remaining_product':'editor/app -> lux_editor, product switch only P12'}
(w/'installation-audit.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('Actual provider names, fresh install manifest and removed headers checked')
rules=json.loads((s/'editor/tests/architecture/rules.json').read_text())['editor_layering']
roots={'metadata':'P12-F/G formal registration and SDK switch', 'plugins':'P12-F/G V7 sidecars and generated providers',
 'context':'P12-B/C/D/F application ownership and formal Host', 'ui':'P12-D/G last legacy UI consumers',
 'tools':'P12-B/D/G formal three tools/settings/project views', 'launcher':'P12-D/F formal launcher composition',
 'app':'P12-E/F/G actual installed executable switch', 'assets':'P12-G equivalent formal persistence tests',
 'transition':'P12-B/F/G delete final private adapters'}
retained_targets={name for name,desc in rules['targets'].items() if desc['layer']=='RETAINED'}
all_text={p:(s/p).read_text(errors='replace') for p in paths if p.endswith(('.hpp','.cpp','.cmake','CMakeLists.txt','.py')) and not p.startswith('dev_log/')}
def scoped_providers(path):
 exact=rules['files'].get(path,[])
 if exact:return exact
 scopes=[(len(desc.get('path','')),name) for name,desc in rules['targets'].items()
         if desc.get('path') and path.startswith(desc['path'].rstrip('/')+'/')]
 return [name for length,name in scopes if length==max((x[0] for x in scopes),default=-1)]
remaining=[]
for p in paths:
 if not p.startswith('editor/'):continue
 providers=rules['files'].get(p,[])
 group=p.split('/')[1]
 if group not in roots and not (set(providers)&retained_targets):continue
 if not providers:
  directory=(s/p).parent
  providers=[x['name'] for x in graph if x['name'] in local_names and Path(x['SOURCE_DIR']).as_posix().endswith(directory.relative_to(s).as_posix())]
 if not providers:
  providers=[name for name,desc in rules['targets'].items() if desc.get('path')=='editor/'+group]
 if not providers:providers=scoped_providers(p)
 logical=p.split('/include/',1)[-1] if '/include/' in p else None
 include_consumers=[]
 if p.endswith(('.hpp','.h')):
  for path,text in all_text.items():
   if path==p:continue
   included=re.findall(r'#\s*include\s*[<"]([^>"\n]+)',text)
   if any(header==logical if logical else Path(header).name==Path(p).name for header in included):
    include_consumers.append(path)
 if not providers:
  providers=sorted({name for consumer in include_consumers for name in scoped_providers(consumer)})
 if not providers and Path(p).name=='CMakeLists.txt':
  providers=[name for name,desc in rules['targets'].items() if desc.get('path','').startswith(str(Path(p).parent).replace('\\','/')+'/')]
 assert providers,('Unresolved actual provider',p)
 remaining.append({'path':p,'blob':subprocess.check_output(['git','rev-parse',sha+':'+p],cwd=s,text=True).strip(),
  'providers':providers,'logical_include':logical,
  'include_consumers':include_consumers,
  'target_consumers':[x['name'] for x in graph if any(re.search(r'(?<!\w)'+re.escape(n)+r'(?!\w)',x.get('edges','')) for n in providers)],
  'declared_types':sorted(set(re.findall(r'\b(?:class|struct|enum class)\s+(?:\w+_PUBLIC\s+)?(\w+)',all_text.get(p,'')))),
  'deadline':'P12','replacement':roots.get(group,'P12-F/G remove old protocol only; keep formal History/Session/activities implementations')})
(w/'retained-file-inventory.json').write_text(json.dumps({'implementation_sha':sha,'scope':'Actual tracked nine roots plus formally located RETAINED-provider files; source-map contains the P11 per-symbol dispositions. Not an independent mutable ledger.','files':remaining},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('P12 handoff exact tracked residual files',len(remaining))
