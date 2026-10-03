from pathlib import Path
import json, subprocess
w=Path(__file__).resolve().parent
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec1')
base='54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8'
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=s,text=True).strip()
changes=subprocess.check_output(['git','diff','--name-status','-M',base,sha],cwd=s,text=True).splitlines()
(w/'file-actions.json').write_text(json.dumps([dict(action=line.split('\t')[0],paths=line.split('\t')[1:]) for line in changes],indent=2)+'\n')
print('actual Git file actions',len(changes))
