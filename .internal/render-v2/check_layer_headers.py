from pathlib import Path
import re,json
import argparse
parser=argparse.ArgumentParser()
parser.add_argument('--source',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
root=args.source.resolve()
headers={}
for p in (root/'modules/function/render').rglob('*.hpp'):
 parts=p.parts
 if 'include' not in parts:continue
 key='/'.join(parts[parts.index('include')+1:]);headers[key]=p
includes={k:re.findall(r'#include\s*[<"]([^>"]+)[>"]',p.read_text()) for k,p in headers.items()}
def closure(key):
 seen=set();pending=[key]
 while pending:
  current=pending.pop()
  if current in seen:continue
  seen.add(current);pending.extend(v for v in includes.get(current,[]) if v in headers)
 return sorted(seen)
probes={
'lux/engine/render/features/Feature.hpp':('/graph/','/resource/','/native/','/protocol/','/runtime/'),
'lux/engine/render/resource/Storage.hpp':('/native/','/runtime/','/features/','/protocol/','/data/Encoding.hpp'),
'lux/engine/render/data/Payload.hpp':('/data/Schema.hpp','/data/Encoding.hpp'),
'lux/engine/render/native/vulkan/CompiledGraph.hpp':('/features/','/runtime/'),
'lux/engine/render/runtime/RenderServer.hpp':('/native/','/runtime/RenderClient.hpp','/runtime/Commands.hpp')}
for name,forbidden in probes.items():
 assert all(not any(word in dep for word in forbidden) for dep in closure(name)),(name,closure(name))
records=[]
for key,p in sorted(headers.items()):
 module=p.relative_to(root/'modules/function/render').parts[0]
 if module in ('core','data','graph') or p.name in ('Feature.hpp','Storage.hpp','Binding.hpp'):category='neutral value / format / graph contract'
 elif module=='native':category='backend-specific external consumer contract'
 elif module=='runtime':category='client/server public API or required installed template implementation'
 else:category='cold registration / algorithm / execution extension contract'
 records.append({'header':key,'category':category,'direct_includes':includes[key]})
result={'headers':records,'probes':{k:closure(k) for k in probes},'status':'PASS'}
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(result,indent=2)+'\n')
print(len(records),'headers; source closure probes PASS')
