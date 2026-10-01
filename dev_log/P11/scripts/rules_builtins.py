from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-p11');p=s/'editor/tests/architecture/rules.json';r=json.loads(p.read_text())
for pth in ['editor/application/extensions/include/lux/engine/editor/extensions/BuiltinContributions.hpp','editor/application/extensions/src/BuiltinContributions.cpp']:r['editor_layering']['files'][pth]=['editor_extensions']
p.write_text(json.dumps(r,indent=2)+'\n')
