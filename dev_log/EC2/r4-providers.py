import json
from pathlib import Path
p=Path('E:/SyncForder/CodeRepos/lux-engine-ec2/editor/tests/architecture/rules.json');j=json.loads(p.read_text())
for file in ['src/MaterialPreviewRecipe.cpp','pinclude/lux/engine/editor/material/MaterialPreviewRecipe.hpp']:
 j['editor_layering']['files']['editor/activities/material/'+file]=['material_preview']
for v in j.values():
 if isinstance(v,dict) and 'headers' in v and 'editor/activities/material/src/MaterialPreview.cpp' in v.get('files',[]):
  for header in ['random','lux/engine/description/Visual.hpp']:
   if header not in v['headers']:v['headers'].append(header)
p.write_text(json.dumps(j,indent=2)+'\n')
