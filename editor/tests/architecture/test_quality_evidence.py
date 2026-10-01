"""Portable archive resolution rejects missing/tampered evidence and path escape."""
from pathlib import Path
import hashlib,tempfile
from validate_quality_evidence import archived
with tempfile.TemporaryDirectory(prefix='lux-p10q-evidence-') as name:
    root=Path(name);data=b'actual evidence bytes';digest=hashlib.sha256(data).hexdigest()
    path=root/'logs'/'result.log';path.parent.mkdir();path.write_bytes(data)
    assert archived(root,'logs/result.log',digest)==data
    for invalid in ['../result.log','C:/production/result.log','logs\\result.log']:
        try:archived(root,invalid,digest)
        except ValueError:pass
        else:raise AssertionError('Accepted nonportable path')
    path.write_bytes(b'tampered')
    try:archived(root,'logs/result.log',digest)
    except ValueError:pass
    else:raise AssertionError('Accepted altered evidence')
    path.unlink()
    try:archived(root,'logs/result.log',digest)
    except FileNotFoundError:pass
    else:raise AssertionError('Skipped missing evidence')
print('P10Q archive path/hash/missing negative cases PASS')
