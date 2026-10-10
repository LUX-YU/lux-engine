"""Independent F2-FIX qualification; no modifications to the qualified clone."""
import argparse, hashlib, json, re, subprocess
from pathlib import Path
BASE='3216bc8d19567abf8fc00c76bab7fdb3fff77e1a'
p=argparse.ArgumentParser()
for key in ('source','build','output','baseline-tests'):p.add_argument('--'+key,type=Path,required=True)
a=p.parse_args();source=a.source.resolve();build=a.build.resolve();out=a.output.resolve();out.mkdir(exist_ok=True,parents=True)
def git(*args):return subprocess.check_output(['git','-c','core.quotepath=false','-C',str(source),*args])
def check(value,message):
 if not value:raise RuntimeError(message)
def norm(v):return str(Path(v).resolve()).replace('\\','/').lower()
def sha(v):return hashlib.sha256(Path(v).read_bytes()).hexdigest()
check(not git('status','--porcelain','--untracked-files=all'),'qualification clone dirty')
check(not build.is_relative_to(source),'build in source')
changed=git('diff','--name-only',BASE,'HEAD').decode().splitlines()
allowed=('modules/function/render/graph/','docs/render-v2/F2_FIX_','cmake/render-v2-bootstrap/verify_f2_fix.py')
conditional={'modules/resource/description/include/lux/engine/description/PassContract.hpp','engine/toolchain/shader/src/PassValidation.cpp'}
check(all(v.startswith(allowed) or v in conditional for v in changed),'scope violation')
protected=['modules/function/render/'+v for v in ('core','transport','vulkan')]+['render_legacy','modules/function/render/graph/cmake']
for folder in ('engine/toolchain/shader','modules/resource/description','cmake/render-v2-bootstrap'):
 protected+=[v for v in git('ls-tree','-r','--name-only',BASE,'--',folder).decode().splitlines() if v not in conditional]
protected+=git('ls-tree','-r','--name-only',BASE,'--','docs/render-v2').decode().splitlines()
for v in protected:check(git('rev-parse',BASE+':'+v)==git('rev-parse','HEAD:'+v),'protected '+v)
legacy={}
for row in git('ls-tree','-rlz','HEAD','--','render_legacy').split(b'\0'):
 if row:
  info,path=row.split(b'\t');mode,kind,blob,size=info.decode().split();legacy[path.decode()]=(mode,blob,int(size))
manifest=json.loads((source/'docs/render-v2/LEGACY_SOURCE_MANIFEST.json').read_text())
check(len(manifest['files'])==719 and len(legacy)==720,'legacy count')
for v in manifest['files']:check(legacy[v['destination']]==(v['mode'],v['blob'],v['bytes']),'legacy '+v['source'])
tracked={norm(source/v) for v in git('ls-files','-z').decode().split('\0') if v}
forbidden=('/render_legacy/','/editor_legacy/','/engine/scene/','/engine/editor/','/render/runtime/','/render/features/')
def input_check(path):
 check(not any(v in path for v in forbidden),'forbidden actual input '+path)
 if path.startswith(norm(source)+'/'):check(path in tracked,'untracked actual input '+path)
 check('/install/debug/' not in path and '/install/relwithdebinfo/' not in path,'old SDK input '+path)
reply=build/'.cmake/api/v1/reply';index=json.loads(max(reply.glob('index-*.json')).read_text())
model=json.loads((reply/index['reply']['codemodel-v2']['jsonFile']).read_text())
targets={}
for ref in model['configurations'][0]['targets']:
 t=json.loads((reply/ref['jsonFile']).read_text());targets[t['id']]=t
names={v['name']:v for v in targets.values()}
for part in ('core','transport','graph','vulkan'):
 t=names['render_'+part];check(t['type']=='STATIC_LIBRARY','real production library '+part)
 deps={targets[d['id']]['name'] for d in t.get('dependencies',[])}
 check(deps==(set() if part=='core' else {'render_core'}),'production link '+part+str(deps))
check({Path(v['path']).name for v in names['render_graph']['sources']}=={'Definition.cpp','Plan.cpp','Bindings.cpp','Builder.cpp'},'Graph actual sources')
for h in ('Authoring','Schema','Builder','Definition','Plan','Bindings','PassContract'):check('render_graph_header_'+h in names,'public TU '+h)
for h in ('PassValidation','lglsl_LglslEmitter'):check('toolchain_shader_header_'+h in names,'shader public TU '+h)
links={};includes={}
for name,t in names.items():
 check(name.startswith(('render_core','render_transport','render_graph','render_vulkan','toolchain_shader','lux_shader_emitter')) or t['type']=='UTILITY','unexpected target '+name)
 inc={norm(v['path']) for g in t.get('compileGroups',[]) for v in g.get('includes',[])}
 for v in inc:input_check(v) if v not in {norm(source/'modules/resource/description/include')} and not Path(v).is_dir() else check(not any(x in v+'/' for x in forbidden),'include root escape')
 libraries=[v['fragment'] for v in t.get('link',{}).get('commandFragments',[]) if v['role']=='libraries']
 links[name]=libraries;includes[name]=sorted(inc)
 if name in ('render_graph','render_core','render_transport') or name.startswith(('render_graph_header_','render_core_header_','render_transport_header_')):
  check(not any('vulkan' in v or 'spirv' in v or '/engine/toolchain/' in v for v in inc),'neutral public include escape '+name)
 if name.startswith('render_graph') and not name.startswith('render_graph_schema_') and name!='render_graph_stage_program':
  check(not any('vulkan' in v.lower() or 'spirv' in v.lower() or 'toolchain' in v.lower() for v in libraries),'graph link escape '+name)
 for g in t.get('compileGroups',[]):
  defs={v['define'] for v in g.get('defines',[])}
  check(('LUX_VULKAN_TEST_SEAM' in defs)==(name=='render_vulkan_fault'),'Vulkan seam escape')
  check(('LUX_RENDER_REPLY_TEST_HOOK' in defs)==(name=='render_transport_reply_lifetime'),'reply seam escape')
commands=json.loads((build/'compile_commands.json').read_text())
for v in commands:input_check(norm(v['file']))
deps=subprocess.check_output(['D:/Softwares/ninja-win/ninja.exe','-C',str(build),'-t','deps'],text=True)
(out/'ninja-dependencies.txt').write_text(deps,encoding='utf-8')
component_headers={v:set() for v in ('core','transport','graph','vulkan')};all_headers=set();part=None
for line in deps.splitlines():
 if line and not line.startswith(' '):
  name=line.replace('\\','/');part=next((v for v in component_headers if name.startswith('render_'+v+'/CMakeFiles/render_'+v+'.dir/')),None)
 elif line.startswith('    '):
  v=Path(line.strip());v=norm(v if v.is_absolute() else build/v);input_check(v);all_headers.add(v)
  if part:component_headers[part].add(v)
for part,headers in component_headers.items():
 check(headers,'missing actual compiler dep '+part)
 if part!='vulkan':check(not any('vulkan' in v or 'vk_mem_alloc' in v or '/spirv' in v for v in headers),'native dependency '+part)
 if part=='graph':
  roots=[source/'modules/function/render/graph',source/'modules/function/render/core',source/'modules/core/error',source/'modules/resource/description/include/lux/engine/description/PassContract.hpp',source/'modules/resource/description/include/lux/engine/description/Image.hpp']
  for v in headers:
   if '/lux/engine/' in v:check(any(v.startswith(norm(x)) for x in roots),'Graph Engine header escape '+v)
cmake_model=json.loads((reply/index['reply']['cmakeFiles-v1']['jsonFile']).read_text());cmake_inputs=[]
for entry in cmake_model['inputs']:
 v=Path(entry['path']);v=norm(v if v.is_absolute() else source/'cmake/render-v2-bootstrap'/v);input_check(v);cmake_inputs.append(v)
jobs=[];codegen_inputs=[]
for jobfile in (build/'render_graph/test/lux_codegen').glob('*.json'):
 job=json.loads(jobfile.read_text());check(job['marker']=='luxpass' and not job['dry_run'],'non-production codegen')
 for v in job['target_files']:input_check(norm(v['physical_path']))
 for v in job['projections']:check(norm(v['template_path'])==norm(source/'modules/function/render/graph/cmake/pass_schema.template'),'projection input')
 jobs.append(job)
for depfile in (build/'render_graph/test/lux_codegen').glob('*.d'):
 for line in depfile.read_text().splitlines()[1:]:
  v=line.strip().removesuffix('\\').strip()
  if v:
   path=norm(re.sub(r'\\(.)',r'\1',v));input_check(path);check('vulkan' not in path,'codegen Vulkan dependency');codegen_inputs.append(path)
check(jobs and codegen_inputs,'missing production codegen evidence')
fixtures=['Tonemap','Blur','Composite','Storage','Hzb','Complex','LocalRead','NonShader','Optional','Attachments','Transfer','HalfStorage','Stages']
generated={}
for fixture in fixtures:
 suffixes=['.schema.json','.pass.hpp','.lglslh']+(['.glsl','.spv'] if fixture in fixtures[:7]+['HalfStorage'] else [])
 for suffix in suffixes:
  f=build/'render_graph/test/generated'/(fixture+suffix);check(f.is_file(),'missing artifact '+str(f));generated[f.name]=sha(f)
 cpp=(build/'render_graph/test/generated'/(fixture+'.pass.hpp')).read_text()
 check('struct PassSchema<::'+fixture+'>' in cpp and 'static_assert(sizeof' in cpp and 'captureResource' in cpp,'generated coverage '+fixture)
for name in ['Stages.vert','Stages.frag','LegacyTonemap.vert']:
 for suffix in ['.glsl','.spv']:
  f=build/'render_graph/test/generated'/(name+suffix);check(f.is_file(),'missing stage artifact');generated[f.name]=sha(f)
ninja=(build/'build.ninja').read_text().lower();check('render_legacy' not in ninja and 'passschema.py' in ninja and 'lux_meta_generator' in ninja and 'glslc' in ninja,'real generation closure')
current_tests=json.loads(subprocess.check_output(['D:/Development/CMake/bin/ctest.exe','--test-dir',str(build),'--show-only=json-v1']))
previous_tests=json.loads(a.baseline_tests.read_text())
old_names={v['name'] for v in previous_tests['tests']};new_names={v['name'] for v in current_tests['tests']}
check(len(old_names)==116 and old_names<=new_names,'original 116 CTest obligations missing')
(out/'tests.json').write_text(json.dumps(current_tests,indent=2))
(out/'test-obligations.json').write_text(json.dumps({'previous':sorted(old_names),'new':sorted(new_names-old_names),'total':len(new_names)},indent=2))
report={'status':'PASS','implementation':git('rev-parse','HEAD').decode().strip(),'base':BASE,'changed_files':changed,'protected_objects':protected,'legacy_count':719,'target_links':links,'target_includes':includes,'compile_commands':commands,'cmake_inputs':cmake_inputs,'production_compiler_headers':{k:sorted(v) for k,v in component_headers.items()},'codegen_jobs':jobs,'codegen_inputs':sorted(set(codegen_inputs)),'generated_sha256':generated,'header_count':len(all_headers)}
(out/'closure.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('PASS: source, include, link, codegen closure;',len(all_headers),'headers;',len(generated),'generated artifacts')
