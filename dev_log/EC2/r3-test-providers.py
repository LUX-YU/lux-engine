import json
from pathlib import Path
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2');p=s/'editor/tests/architecture/rules.json';j=json.loads(p.read_text())
for target,file,caps in [('editor_project_panels','panels.cpp',['CPU','PROCESS','GPU']),('editor_artifact_publication','artifact_publication.cpp',['CPU','PROCESS','TOOLCHAIN'])]:
 j['editor_layering']['targets'][target]={'layer':'TEST','role':'TEST','capabilities':caps,'path':'editor/tests/integration/project'}
 j['editor_layering']['files']['editor/tests/integration/project/'+file]=[target]
p.write_text(json.dumps(j,indent=2)+'\n',newline='\n')
