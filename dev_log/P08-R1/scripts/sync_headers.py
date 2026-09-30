from pathlib import Path
import argparse,json,shutil,hashlib
parser=argparse.ArgumentParser();parser.add_argument('--repo',type=Path,required=True);parser.add_argument('--install',type=Path,required=True);args=parser.parse_args()
rows=[]
headers=['modules/core/object/include/lux/engine/object/LuxObject.hpp']+['modules/function/ui/include/lux/engine/ui/'+x+'.hpp' for x in ['Root','Pane','Element','Attachment']]
for source in headers:
 for prefix in ['Debug/include','RelWithDebInfo/include','Android/lux-engine/include']:
  destination=args.install/prefix/source.split('/include/',1)[1]
  destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(args.repo/source,destination)
  raw=destination.read_bytes()
  rows.append(dict(source=source,prefix=prefix,copied=raw==(args.repo/source).read_bytes(),sha256=hashlib.sha256(raw.replace(b'\r\n',b'\n')).hexdigest()))
print(json.dumps(rows,indent=2))
