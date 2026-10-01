from pathlib import Path
import os,subprocess,json,shutil,hashlib,argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--prefix',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--build-root',type=Path,required=True);p.add_argument('--only',default='');a=p.parse_args()
repo=a.source.resolve();prefix=a.prefix.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);base=a.build_root.resolve();base.mkdir(parents=True,exist_ok=True)
os.environ['PATH']=str(prefix/'bin')+os.pathsep+'D:/Development/vcpkg/installed/x64-windows/bin'+os.pathsep+os.pathsep.join(v for v in os.environ['PATH'].split(os.pathsep) if '/install/' not in v.replace('\\','/').lower() and '/lux-engine/bin' not in v.replace('\\','/').lower() and '/coderepos/build/' not in v.replace('\\','/').lower())
records=json.loads((out/'commands.json').read_text()) if (out/'commands.json').exists() else []
def run(name,args,cwd=repo):
 args=list(map(str,args));log=out/(name+'.log');version=1
 while log.exists():log=out/(name+f'-{version}.log');version+=1
 with log.open('wb') as stream:r=subprocess.run(args,cwd=cwd,stdout=stream,stderr=subprocess.STDOUT)
 records.append({'name':name,'argv':args,'exit_code':r.returncode,'log':log.name,'sha256':hashlib.sha256(log.read_bytes()).hexdigest()});(out/'commands.json').write_text(json.dumps(records,indent=2))
 print(name,r.returncode,flush=True)
 if r.returncode:print(log.read_text(errors='replace')[-7000:]);raise SystemExit(r.returncode)
common=['-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_CXX_STANDARD=20','-DCMAKE_CXX_EXTENSIONS=OFF',f'-DCMAKE_PREFIX_PATH={prefix.as_posix()}','-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake','-DVCPKG_TARGET_TRIPLET=x64-windows','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
# Each group uses maintained test bodies and installed production API only.
groups={
 'quality':('quality',[]),'sessions':('editor-sessions',[]),'editor-d2':('editor-d2',[]),
 'external-feature':(None,[f'-DLUX_PLUGIN_CMAKE_DIR={prefix.as_posix()}/share/lux-engine/plugins','-DLUX_SAMPLE_EDITOR=ON']),
 'scene-ui':('scene-ui',['-DCONSUMER_MODE=CPU_UI']),'views-ui':('scene-ui',['-DCONSUMER_MODE=CPU_UI']),
 'scene-model':('scene-model',[]),'material-model':('material-model',[]),'flowforge-model':('flowforge-model',[]),
 'persistence':('persistence',[]),'scene-execution':('scene-execution',[]),'projection-compilation':('projection-compilation',[]),
 'interaction-views':('interaction-views',[]),'workspace':('workspace',[]),'desktop-views':('desktop-views',[]),
 **{'layering-'+mode.lower():('layering',['-DCONSUMER_MODE='+mode]) for mode in ['TASKS','SAVE_CORE','PROJECT','LAYOUT']},
 'gpu-ui':('scene-ui',['-DCONSUMER_MODE=GPU_UI']),'editor-scene-pane':('scene-ui',['-DCONSUMER_MODE=EDITOR_SCENE_PANE'])}
copies={
 'persistence':[('editor/tests/persistence/models.cpp','main.cpp')],
 'scene-execution':[('editor/activities/scene/test/runs.cpp','main.cpp')],
 'projection-compilation':[('editor/tests/integration/projection/projection.cpp','projection.cpp'),('editor/tests/integration/material_activity/compilation.cpp','compilation.cpp'),('editor/tests/integration/material_activity/ownership.cpp','ownership.cpp')],
 'interaction-views':[('editor/workbench/desktop/test/lifecycle.cpp','lifecycle.cpp'),('editor/tests/persistence/interaction_reclaim.cpp','interaction_reclaim.cpp')],
 'workspace':[('editor/tests/workspace/workspace.cpp','workspace.cpp'),('editor/tests/workspace/effects.cpp','effects.cpp')]}
for name,(folder,extra) in groups.items():
 if a.only and name not in a.only.split(','):continue
 source=repo/'cmake/installed-consumers'/folder if folder else prefix/'share/lux-engine/examples/external-feature'
 if name in copies:
  target=base/(name+'-source');shutil.copytree(source,target,dirs_exist_ok=True)
  for origin,dest in copies[name]:shutil.copyfile(repo/origin,target/dest)
  source=target
 build=base/name
 run(name+'-configure',['cmake','-S',source,'-B',build,*common,*extra])
 for step in ['build','no-work']:run(name+'-'+step,['cmake','--build',build,'--target','all','-j','4','--','-k','0'])
 run(name+'-ctest',['ctest','--test-dir',build,'--output-on-failure','-j','1'])
 shutil.copyfile(build/'Testing/Temporary/LastTest.log',out/(name+'-ctest-details.log'))
 units=json.loads((build/'compile_commands.json').read_text())
 for unit in units:
  command=unit['command'].replace('\\','/').lower()
  assert '/pinclude/' not in command and '/sinclude/' not in command
  assert '/install/relwithdebinfo/include' not in command,command
 (out/(name+'-compile-commands.json')).write_text(json.dumps(units,indent=2))
 for log in build.glob('reject_*.log'):shutil.copyfile(log,out/(name+'-'+log.name))
