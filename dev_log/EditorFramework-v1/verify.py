"""Verify a portable receipt using only its relative manifest. Never consult production paths."""
from pathlib import Path
import hashlib,json,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path(__file__).resolve().parent
manifest=json.loads((root/'manifest.json').read_text(encoding='utf-8'))
for relative,expected in manifest['files'].items():
    path=(root/relative).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise SystemExit('Missing or invalid evidence: '+relative)
    if hashlib.sha256(path.read_bytes()).hexdigest()!=expected:
        raise SystemExit('Evidence hash mismatch: '+relative)
print('PASS portable receipt:',manifest['implementation_sha'],len(manifest['files']),'files')
