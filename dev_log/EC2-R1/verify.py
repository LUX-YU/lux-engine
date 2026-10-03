"""Verify the archived EC2 R1 evidence without opening any production/build path."""
from pathlib import Path
import argparse, hashlib, json, subprocess
p=argparse.ArgumentParser();p.add_argument('--archive',type=Path,default=Path(__file__).resolve().parent)
p.add_argument('--repo',type=Path,required=True);a=p.parse_args();w=a.archive
manifest=json.loads((w/'manifest.json').read_text())
for name,digest in manifest.items():
 path=w/name
 assert path.is_file(),f'missing evidence: {name}'
 assert hashlib.sha256(path.read_bytes()).hexdigest()==digest,f'changed evidence: {name}'
r=json.loads((w/'receipt.json').read_text());commands={x['name']:x for x in json.loads((w/'commands.json').read_text())}
sha=r['implementation_sha'];base=r['baseline_sha']
subprocess.run(['git','merge-base','--is-ancestor',base,sha],cwd=a.repo,check=True)
for x in commands.values():
 assert hashlib.sha256((w/x['log']).read_bytes()).hexdigest()==x['sha256'],x['name']
required=['final-tracked','final-configure','final-build','final-no-work','final-ctest','final-install',
 'final-sdk','final-headers','final-public-recipe-configure','final-public-recipe-build']
for name in required:
 x=commands[name];assert x['exit_code']==0 and x['source_head']==sha and x['source_head_after']==sha,name
 assert x['source_diff_sha256']==hashlib.sha256(b'').hexdigest(),name
 assert not x['source_untracked_sha256'],name
assert commands['before-sdk-configure']['exit_code']==0
assert commands['before-sdk-build']['exit_code']!=0
assert 'MaterialPreviewRecipe' in (w/commands['before-sdk-build']['log']).read_text(errors='replace')
assert 'no work to do' in (w/commands['final-no-work']['log']).read_text(errors='replace')
assert '-DLUX_EDITOR_MIGRATION_STAGE=EC2' in commands['final-configure']['argv']
assert '-DLUX_EDITOR_LAYERING_MODE=STRICT' in commands['final-configure']['argv']
assert 'EC2-R1 XEC2-12 public sphere/quad recipe GPU readbacks differ' in (w/'final-ctest-details.log').read_text(errors='replace')
assert 'EC2-R1 XEC2-12 public sphere/quad recipe GPU readbacks differ' in (w/'sdk/desktop-views-ctest-details.log').read_text(errors='replace')
assert 'validation_errors=0' in (w/'sdk/desktop-views-ctest-details.log').read_text(errors='replace')
for x in json.loads((w/'sdk/commands.json').read_text()):
 assert x['exit_code']==0,x['name']
 assert hashlib.sha256((w/'sdk'/x['log']).read_bytes()).hexdigest()==x['sha256'],x['name']
old_tree=subprocess.check_output(['git','rev-parse',base+':dev_log/EC2'],cwd=a.repo,text=True).strip()
new_tree=subprocess.check_output(['git','rev-parse',sha+':dev_log/EC2'],cwd=a.repo,text=True).strip()
assert old_tree==new_tree==r['original_ec2_tree']
assert r['native_input']=='NOT_RUN_USER_DEFERRED' and not r['user_patch_applied']
assert r['status']=='PASS_SCOPED_EC2_R1' and r['ec2_status']=='PARTIAL'
print('EC2 R1 archived commands, real GPU/source+SDK, preserved history and source SHA verified:',sha)
