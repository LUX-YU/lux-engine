from pathlib import Path
import json

work=Path(__file__).parent
original=work.parent
sdk=(original/'run_sdk_p10q.py').read_text()
plan=json.loads((work/'file-plan.json').read_text())
for item in sorted(plan,key=lambda x:-len(x['source'])):
 if item['source']!=item['destination']:
  sdk=sdk.replace(item['source'],item['destination'])
sdk=sdk.replace("'gpu-ui':('scene-ui'", "**{'layer-'+mode.lower():('layering',['-DCONSUMER_MODE='+mode]) for mode in ['TASKS','SAVE_CORE','PROJECT','LAYOUT']},\n 'gpu-ui':('scene-ui'")
sdk=sdk.replace("'/lux-engine/bin' not in v.replace('\\\\','/').lower()", "'/lux-engine/bin' not in v.replace('\\\\','/').lower() and '/coderepos/build/' not in v.replace('\\\\','/').lower()")
sdk=sdk.replace("records=[]", "records=json.loads((out/'commands.json').read_text()) if (out/'commands.json').exists() else []")
(work/'run_sdk.py').write_text(sdk)

q=(original/'qualify_p10q.py').read_text()
q=q.replace("a=p.parse_args();workspace=Path(__file__).resolve().parents[2];cluster=workspace.parent", "a=p.parse_args();workspace=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure');cluster=workspace.parent")
q=q.replace("work=workspace/'.internal/editor-redesign';out=work/'P10Q-final'/sha", "work=Path(__file__).resolve().parent;out=work/'final'/sha")
q=q.replace("('p10q-clean-'+sha[:12])", "('p10q-structure-clean-'+sha[:12])")
q=q.replace("('p10q-'+sha[:12])", "('p10q-structure-'+sha[:12])")
q=q.replace("('P10Q-'+sha[:12])", "('P10Q-structure-'+sha[:12])")
q=q.replace("'/lux-engine/bin' not in v.replace('\\\\','/').lower()", "'/lux-engine/bin' not in v.replace('\\\\','/').lower() and '/coderepos/build/' not in v.replace('\\\\','/').lower()")
q=q.replace("'-DLUX_EDITOR_MIGRATION_STAGE=P10Q'", "'-DLUX_EDITOR_MIGRATION_STAGE=P10Q','-DLUX_EDITOR_LAYERING_MODE=STRICT'")
q=q.replace("(work/'P10Q-sdk-dependencies.json')", "(work.parent/'P10Q-sdk-dependencies.json')")
q=q.replace("data=src.read_bytes()", "data=src.read_bytes()\n        assert hashlib.sha256(data).hexdigest()==item['sha256'], str(src)")
q=q.replace("work/'run_sdk_p10q.py'", "work/'run_sdk.py'")
q=q.replace("work/'check_headers_p10q.py'", "work/'check_headers.py'")
# Evidence of dirty/untracked source must precede the independent clone, never be hidden by it.
q=q.replace("if a.phase=='cold':\n", "if a.phase=='cold':\n    assert not subprocess.check_output(['git','status','--porcelain'],cwd=workspace,text=True).strip(), 'Qualification requires clean implementation checkout'\n")
(work/'qualify.py').write_text(q)

h=(original/'check_headers_p10q.py').read_text()
h=h.replace("repo=Path(__file__).resolve().parents[2];work=repo/'.internal/editor-redesign';cluster=repo.parent", "repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure');work=Path(__file__).resolve().parent;cluster=repo.parent")
h=h.replace("('P10Q-'+sha[:12])", "('P10Q-structure-'+sha[:12])")
h=h.replace("out=work/'P10Q-final'/sha/'public-headers'", "out=work/'final'/sha/'public-headers'")
h=h.replace("b583e7ffe20e7a1ac55c7119d6a13ac337ebb323", "f7c27f9375cbf8dd8af37b30a6027a460de26213")
h=h.replace("'/Zs'", "'/Zs'")
(work/'check_headers.py').write_text(h)
print('Qualification and SDK runners use the isolated implementation SHA, unique ledger and fresh dependency-only prefix.')
