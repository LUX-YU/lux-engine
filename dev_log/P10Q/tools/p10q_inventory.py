from pathlib import Path
import json,subprocess,argparse
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
reply=a.build/'.cmake/api/v1/reply'
index=json.loads(max(reply.glob('index-*.json'),key=lambda p:p.stat().st_mtime).read_text())
model=json.loads((reply/index['reply']['codemodel-v2']['jsonFile']).read_text())
result={'source':model['paths']['source'],'build':model['paths']['build'],'targets':[]}
for entry in model['configurations'][0]['targets']:
    target=json.loads((reply/entry['jsonFile']).read_text())
    result['targets'].append({k:target[k] for k in ['name','type','paths','artifacts','dependencies','link'] if k in target})
result['editor_shared']=[t['name'] for t in result['targets'] if t['type']=='SHARED_LIBRARY' and t['paths']['source'].startswith('editor/')]
result['editor_static']=[t['name'] for t in result['targets'] if t['type']=='STATIC_LIBRARY' and t['paths']['source'].startswith('editor/')]
result['sdk_packages']=sorted(p.parent.name for p in a.build.rglob('*-config.cmake') if '/_deps/' not in str(p).replace('\\','/'))
a.out.write_text(json.dumps(result,indent=2))
print('editor shared:',result['editor_shared']);print('editor static count:',len(result['editor_static']))
