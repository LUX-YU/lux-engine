from pathlib import Path
import subprocess,json,hashlib
root=Path(__file__).resolve().parent
src=root/'MaterialInteraction.cpp'; b=src.read_bytes()
sha=hashlib.sha1(f'blob {len(b)}\0'.encode()+b).hexdigest()
assert sha=='6bb3c27ebc361d65c4fb319b86bb9387b3e68383'
runs=[]
for opt in ['-O0','-O2']:
    binary=root/('probe'+opt[1:])
    cmd=['g++','-std=c++23',opt,'-Wall','-Wextra','-Werror','-I',str(root/'include'),str(src),str(root/'main.cpp'),'-o',str(binary)]
    cp=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
    assert cp.returncode==0,(cmd,cp.stdout,cp.stderr)
    for mode in ['busy-cancel','busy-sync','live-cancel','stale-sync']:
        r=subprocess.run([str(binary),mode],capture_output=True,text=True,timeout=5)
        item={'optimization':opt,'scenario':mode,'compile_command':cmd,'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
        (root/(opt[1:]+'_'+mode+'.log')).write_text(r.stdout+r.stderr)
        runs.append(item)
        print(r.stdout.strip())
result={'scope':'Original MaterialInteraction.cpp Git blob compiled against explicit isolated access/model shim. BUSY is injected; real SessionStore/SDK/UI not compiled or run. C++23 only supplies std::expected in the shim, not a requested project standard change. Exit 1 denotes the preserved-state target contract failing.',
        'git_blob':sha,'compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0], 'runs':runs}
(root/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2))
assert all(x['exit_code']==(1 if x['scenario'].startswith('busy') else 0) for x in runs)
