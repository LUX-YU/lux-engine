"""Archive this stage's raw evidence only; compiled images remain local with immutable hash inventories."""
import hashlib, json, os, sys, zipfile
from pathlib import Path
root=Path(sys.argv[1]).resolve();destination=Path(sys.argv[2]).resolve()
if not (root/'ready-to-archive.json').exists(): raise RuntimeError('Serial qualification/measurement must finish first')
destination.mkdir(parents=True,exist_ok=True)
groups={name:[] for name in ['qualification','identities','installation','costs','vtune']}
text_suffixes={'.log','.json','.xml','.csv','.txt','.md','.cmake','.cpp','.hpp','.h','.c','.lua','.py','.ps1','.template','.in'}
for directory,children,files in os.walk(root):
    path=Path(directory);relative=path.relative_to(root);parts=relative.parts
    if not parts:
        children[:]=[name for name in children if name!='instructions']
    if parts==('dependencies',):
        children[:]=[name for name in children if name=='build-lua55']
    elif parts==('dependencies','build-lua55'):
        children[:]=[name for name in children if name=='Testing']
    if parts and parts[0]=='relocated' and len(parts)==1:
        children[:]=[name for name in children if name not in ['sdk','tools','vm']]
    for name in files:
        file=path/name
        if len(parts)>=2 and parts[0].startswith('profile55') and parts[1]=='result':
            assert file.suffix.lower() not in ['.exe','.dll','.pdb','.lib','.obj','.o','.a','.so','.dylib'], str(file)
            groups['vtune'].append(file)
            continue
        if file.suffix not in text_suffixes and name not in ['build.ninja','.ninja_log']: continue
        if name=='raw-files.json' or name=='archives.json': continue
        if parts and parts[0] in ['images','original-images']:
            if name not in ['identity.json','CMakeCache.txt','compile_commands.json']: continue
            group='identities'
        elif parts and parts[0]=='dependencies':
            if len(parts)>1 and name not in ['CMakeCache.txt','compile_commands.json','build.ninja','LastTest.log']: continue
            group='identities'
        elif parts and (parts[0].startswith('consumer') or parts[0]=='relocated' or parts[0].startswith('vm-')):
            group='installation'
        elif parts and (parts[0] in ['costs','memory'] or parts[0].startswith('profile55')):
            group='costs'
        else: group='qualification'
        groups[group].append(file)
entries=[];archives=[]
for group,files in groups.items():
    target=destination/(group+'.zip')
    if target.exists(): raise RuntimeError('Refusing archive overwrite '+str(target))
    with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for file in sorted(files):
            relative=file.relative_to(root).as_posix()
            data=file.read_bytes()
            entries.append(dict(archive=target.name,path=relative,bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
            archive.writestr(relative,data)
    assert target.stat().st_size<90_000_000,'Split oversized evidence rather than exceeding hosting limits'
    archives.append(dict(path=target.name,bytes=target.stat().st_size,sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
    with zipfile.ZipFile(target) as archive:
        for record in [e for e in entries if e['archive']==target.name]:
            assert hashlib.sha256(archive.read(record['path'])).hexdigest()==record['sha256']
    print(group,len(files),target.stat().st_size,flush=True)
(destination/'raw-files.json').write_text(json.dumps(entries,indent=2))
(destination/'archives.json').write_text(json.dumps(archives,indent=2))
(destination/'SHA256SUMS').write_text(''.join(f"{a['sha256']}  {a['path']}\n" for a in archives))
print('VERIFIED',len(entries),'raw entries; compiled files excluded')
