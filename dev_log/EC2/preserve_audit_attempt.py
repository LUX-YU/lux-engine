"""Label an unfrozen failed attempt explicitly; preserve original bytes and original record."""
from pathlib import Path
import hashlib,json
w=Path(__file__).resolve().parent
p=w/'commands.json';records=json.loads(p.read_text())
r=next(x for x in records if x['name']=='final-installed-audit')
assert r['exit_code']==1
original=dict(r)
(w/'before/installed-audit-original-command.json').write_text(json.dumps(original,indent=2)+'\n')
old=w/r['log'];new=w/'logs/final-installed-audit-discovery-failure.log'
assert not new.exists() and hashlib.sha256(old.read_bytes()).hexdigest()==r['sha256']
old.rename(new)
r['name']='final-installed-audit-discovery-failure'
r['log']=new.relative_to(w).as_posix()
r['attempt_label_note']='Original record retained in before/installed-audit-original-command.json. Exit status and log bytes unchanged; discovery glob corrected before actual final rerun.'
p.write_text(json.dumps(records,indent=2)+'\n')
