"""Freeze this node only after R9 qualification; historical snapshots are never rewritten."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib,json,shutil,subprocess,sys
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text())
s=Path(c['review_source']);q=Path(c['source']);sha=c['implementation_sha']
for root in (s,q):
    assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()==sha
    assert not subprocess.check_output(['git','status','--porcelain'],cwd=root).strip()
subprocess.run([sys.executable,str(w/'coverage_plan.py')],check=True)
coverage=json.loads((w/'coverage.json').read_text())
assert [(x['id'],x['status']) for x in coverage if x['status']!='PASS']==[('XEC2-37','PARTIAL')]
assert '待完成最终资格' not in (w/'report.md').read_text()
receipt=dict(phase='EC2',migration_stage='EC2',layering_mode='STRICT',stop_after='EC2',status='PARTIAL',
    input_sha='248adc4576943cab83976afd8d1d5f31b63b70a9',implementation_sha=sha,user_patch_applied=False,
    frozen_at=datetime.now(timezone.utc).isoformat(),checkout=str(s),qualification_checkout=str(q),
    pending=dict(native_input='NOT_RUN_USER_DEFERRED',reason='User selected 稍后再做 for source and installed SDK native input; this is postponement, not waiver.'),
    inherited=dict(P12='PARTIAL_USER_WAIVER',linux='NOT_RUN',system_ime='NOT_RUN',
                   old_50k='PARTIAL_NO_MORE_SAMPLES',android_build='NOT_RUN'),
    scope='EC2 R0-R9 Windows/real Lua/PLAYER/SDK; EC1 responsibility audit RA01-09 implemented, RA10 inherited. '
          'No main merge, branch deletion or release. See report for actual joint Run/GPU coverage and unsupported bindings.')
(w/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
ledger_path=w.parent/'migration-ledger.json';ledger=json.loads(ledger_path.read_text(encoding='utf-8-sig'))
node=ledger['ec2'];node['status']='IMPLEMENTED_VALIDATION_PARTIAL_WAITING_REVIEW';node['implementation_sha']=sha
node['next_entry_and_known_blocks']='Native source/SDK input deferred at user request. Stop at EC2 review; no following phase authorized.'
node['batches']['R0']={'status':'COMPLETE','evidence':'EC2/r0-decisions.md; baseline/input/source-consumers'}
for key in ['R1','R2','R3','R4','R5','R6','R7','R8']:
    node['batches'][key]['final_qualification']='PASS_FINAL_'+sha
node['batches']['R9']=dict(status='PARTIAL',implementation_sha=sha,archive='dev_log/EC2/',
    source='clean independent clone',stage='EC2',layering='STRICT',coverage='37 PASS; XEC2-37 PARTIAL, native input deferred')
node['final']=dict(archive='dev_log/EC2/',review_checkout=str(s),qualification_checkout=str(q),
    user_patch_applied=False,public_header_sync='Three prefixes; Android build NOT_RUN',
    untested=['EC2 native input postponed by user','Linux','system IME','Android build','old waived slow benchmark samples'])
ledger_path.write_text(json.dumps(ledger,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
(w/'ledger-snapshot.json').write_bytes(ledger_path.read_bytes())
archive=w/'archive';assert not archive.exists(),'Do not overwrite a frozen archive'
archive.mkdir();(archive/'.gitattributes').write_text('* -text\n')
for p in w.iterdir():
    if p.is_file() and p.suffix in ('.py','.json','.md','.txt','.log'):
        shutil.copy2(p,archive/p.name)
for name in ['input','logs','before','after','sdk','public-headers','build-proof','dll-proof','external-artifact','script-views']:
    assert (w/name).is_dir(),name
    shutil.copytree(w/name,archive/name)
for name in ['LUX_ENGINE_EC1_RESPONSIBILITY_AUDIT_2026-10-02.zip',
             'LUX_ENGINE_EC2_RESPONSIBILITIES_AND_GAME_SCRIPT_2026-10-03.zip']:
    shutil.copy2(Path('C:/Users/ChenHui/Downloads')/name,archive/name)
artifacts=[dict(path=p.relative_to(archive).as_posix(),sha256=hashlib.sha256(p.read_bytes()).hexdigest())
    for p in sorted(archive.rglob('*')) if p.is_file()]
(archive/'artifacts.json').write_text(json.dumps(artifacts,indent=2)+'\n')
print('EC2 archive staged',sha,len(artifacts),'files; portable proof and Git record commit still required')
