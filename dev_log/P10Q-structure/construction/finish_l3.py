from pathlib import Path
import json

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
p=repo/'editor/tests/architecture/rules.json'
r=json.loads(p.read_text())
for name in ('material_ui','flow_ui'):
 r[name]['project_include_dirs']=['editor/workbench/sinclude' if x=='editor/editing/sinclude' else x for x in r[name].get('project_include_dirs',[])]
r['view_api']['files'].append('editor/workbench/desktop/include/lux/engine/editor/views/ViewError.hpp')
r['view_api']['headers'].append('lux/engine/editor/views/ViewError.hpp')
r['layout_model']['files'].append('editor/authoring/layout/include/lux/engine/editor/workspace/RecoveryManifest.hpp')
# Recovery is a value owned by the actual layout target, not a separate library.
r['targets']=[t for t in r['targets'] if t['name']!='recovery_model']
r.pop('recovery_model')
for t in r['targets']:
 t['dependencies']=list(dict.fromkeys('layout_model' if x=='recovery_model' else x for x in t['dependencies']))
p.write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
p=repo/'editor/tests/architecture/test_workspace_boundaries.py'
p.write_text(p.read_text().replace('"recovery_model"','"layout_model"'))
p=repo/'editor/tests/architecture/check_editor_boundaries.py'
s=p.read_text().replace('["layout_model", "recovery_model", "workspace_store"]','["layout_model", "workspace_store"]')
s=s.replace('is_factory = path.startswith(("editor/tools/", "editor/workbench/project/", "editor/workbench/tasks/")) and "/ui/" in path',
 '''is_factory = (path.startswith("editor/tools/") and "/ui/" in path) or path.startswith((
                "editor/workbench/scene/", "editor/workbench/material/", "editor/workbench/flow/",
                "editor/workbench/project/", "editor/workbench/tasks/"))''')
old='''            # Tool roots also contain the independent UI/model targets. The narrowest owner is
            # authoritative; declaration order must not give a nested unit its legacy root policy.
            target = max((t for t in rules["targets"] if path.startswith(t["path"] + "/")),
                         key=lambda t: len(t["path"]))'''
new='''            # Co-located targets have different responsibilities. Resolve the actual compile
            # target first; a directory cannot establish ownership of a translation unit.
            candidates = [t for t in rules["targets"] if t["name"] in targets and
                          path in owned_sources(repo, targets[t["name"]], rules.get(t["name"], {}))]
            compiled = re.search(r"CMakeFiles/([^/]+)\\.dir/", command)
            target = next((t for t in candidates if compiled and t["name"] == compiled[1]), None)
            if target is None:
                if len(candidates) != 1:
                    report("UNRESOLVED_COMPILE_OWNER", path, [t["name"] for t in candidates])
                    continue
                target = candidates[0]'''
assert old in s
p.write_text(s.replace(old,new))
w=Path(__file__).parent;p=w/'ledger.json';r=json.loads(p.read_text())
r['batch']='L3';r['next']='Workbench closure, exact header installation and actual target layering rules.'
r['implementation_commits'].append({'sha':'53a64c186','scope':'L2 activities and CPU task observer'})
r['verification'].append({'batch':'L2','status':'PASS_DEVELOPMENT','evidence':['runs/L2-activities-tests.log','runs/L2-serial-confirm-no-work.log','runs/L2-final-monitor.log'],'notes':'Full all build; 70 regressions, CPU-only monitor. Two final-format build invocations briefly overlapped; neither is no-work proof. Serial-confirm log is authoritative.'})
r['decisions'].append({'id':'D11','decision':'RecoveryManifest belongs to actual layout_model; remove phantom recovery_model rule and exercise the same negative cases on the real target.'})
p.write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
