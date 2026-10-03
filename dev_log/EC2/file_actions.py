"""Final Git-backed file inventory; include consumers are evidence, not inferred link proofs."""
from pathlib import Path
import hashlib, json, re, subprocess
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text());s=Path(c['source'])
base='248adc4576943cab83976afd8d1d5f31b63b70a9';sha=c['implementation_sha']
def git(*args):return subprocess.check_output(['git',*args],cwd=s).decode()
tracked=git('ls-files','editor','engine','modules','cmake').splitlines()
texts={n:(s/n).read_text(errors='replace') for n in tracked if Path(n).suffix in ('.cpp','.hpp','.h','.cmake','.txt','.py')}
rows=[]
for line in git('diff','--name-status','-M',base,sha).splitlines():
    fields=line.split('\t');state=fields[0];old=fields[1];new=fields[-1]
    logical=new.split('/include/',1)[1] if '/include/' in new else None
    matches=[n for n,t in texts.items() if logical and logical in t and n!=new]
    role=('test/generator/build qualification' if any(x in new for x in ('/test/','/tests/','installed-consumers'))
          else 'build provider' if new.endswith('CMakeLists.txt') or new.startswith('cmake/')
          else 'pure authoring values' if new.startswith('editor/authoring/')
          else 'activity execution/authority' if new.startswith('editor/activities/')
          else 'presentation' if new.startswith('editor/workbench/')
          else 'product composition' if new.startswith('editor/application/')
          else 'engine runtime capability' if new.startswith('engine/')
          else 'shared primitive/transport' if new.startswith('modules/') else 'documentation')
    rows.append(dict(action=state,old_path=old,path=new,role=role,logical_include=logical,
        include_consumers=matches,sha256=hashlib.sha256((s/new).read_bytes()).hexdigest() if (s/new).is_file() else None))
(w/'file-actions.json').write_text(json.dumps(rows,indent=2)+'\n')
print('Actual file actions',len(rows),'public include consumer sets',sum(bool(x['logical_include']) for x in rows))
