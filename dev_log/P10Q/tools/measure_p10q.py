from pathlib import Path
import subprocess,os,json,hashlib,sys,argparse
parser=argparse.ArgumentParser();parser.add_argument('--before',action='store_true');args=parser.parse_args()
repo=Path(__file__).resolve().parents[2];cluster=repo.parent;work=repo/'.internal/editor-redesign'
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
root=cluster/'build/RelWithDebInfo'/('p10q-'+sha[:12]);prefix=cluster/'install'/('P10Q-'+sha[:12])
out=work/'P10Q-before/extended' if args.before else work/'P10Q-final'/sha/'performance';out.mkdir(parents=True,exist_ok=True)
source=cluster/'build'/('p05-r1-clean-f2b5a00c60e9' if args.before else 'p10q-clean-'+sha[:12])
if args.before:prefix=cluster/'install/RelWithDebInfo'
os.environ['PATH']=str(prefix/'bin')+os.pathsep+'D:/Development/vcpkg/installed/x64-windows/bin'+os.pathsep+os.environ['PATH']
records_file=out/'commands.json'
records=json.loads(records_file.read_text()) if records_file.exists() else []
measured_sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip()
def run(name,args,cwd=repo):
    args=list(map(str,args));log=out/(name+'.log');i=1
    prior=next((x for x in records if x['name']==name),None)
    if prior and prior['exit_code']==0 and prior['argv']==args:
        assert hashlib.sha256((out/prior['log']).read_bytes()).hexdigest()==prior['sha256']
        print(name,'verified same-input evidence',flush=True);return
    while log.exists():log=out/(name+f'-{i}.log');i+=1
    with log.open('wb') as stream:r=subprocess.run(args,cwd=cwd,stdout=stream,stderr=subprocess.STDOUT)
    if prior:prior['name']+='-attempt-'+str(i-1)
    records.append({'name':name,'argv':args,'exit_code':r.returncode,'log':log.name,'implementation_sha':measured_sha,
        'sha256':hashlib.sha256(log.read_bytes()).hexdigest()})
    (out/'commands.json').write_text(json.dumps(records,indent=2));print(name,r.returncode,flush=True)
    if r.returncode:print(log.read_text(errors='replace')[-6000:]);raise SystemExit(r.returncode)
bench=root.with_name('p10q-before-extended' if args.before else root.name+'-measure')
run('configure',['cmake','-S',work/'P10Q-bench','-B',bench,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
    '-DCMAKE_PREFIX_PATH='+str(prefix),'-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
    '-DVCPKG_TARGET_TRIPLET=x64-windows','-DQUALITY_AFTER='+('OFF' if args.before else 'ON'),
    '-DBASELINE_SOURCE='+str(source)])
run('build',['cmake','--build',bench,'--target','all','-j','4','--','-k','0'])
for count in [1000,10000,50000]:
    for shape in ['chain','wide','roots','cycle']:
        if not args.before:run(f'bq1-{count}-{shape}',[root/'bin/editor_hierarchy_benchmark.exe',count,shape,100],root/'bin')
        elif (count,shape)!=(50000,'chain'):
            run(f'bq1-{count}-{shape}',[cluster/'build/RelWithDebInfo/p10q-bq1-before/bq1.exe',count,shape,100])
        run(f'bq1-alloc-{count}-{shape}',[bench/'hierarchy_alloc.exe',count,shape,0])
for mode in ['catalog','tasks','tasks-revision']:
    for count in [1000,10000]:
        for views in [1,2,8]:run(f'{mode}-{count}-{views}',[bench/'measure.exe',mode,count,views])
run('bytes',[bench/'measure.exe','bytes'])
run('canvas-ids',[bench/'canvas_ids.exe'])
run('candidate-paths',[bench/'candidates.exe'])
if not args.before:
    run('canvas',[bench/'canvas_trace.exe'])
    run('owners',[bench/'owners.exe'])
run('records',[bench/'records.exe','measure'])
if not args.before:
    run('parent-counters',[sys.executable,work/'count_hierarchy_p10q.py'])
