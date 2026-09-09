"""Apply Lux leaf-yield to a verified COPY; official files and archive stay untouched."""
import argparse, hashlib, json, shutil, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--official',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--patch',type=Path,required=True)
a=p.parse_args()
o=a.official.resolve(strict=True); out=a.output.resolve(); patch=a.patch.resolve(strict=True)
identity=json.loads(Path(str(o)+'.identity.json').read_text())
expected='1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce'
if identity['archive_sha256']!=expected: raise SystemExit('Unverified official identity')
for name,digest in identity['files'].items():
 f=(o/name).resolve(strict=True)
 if not f.is_relative_to(o) or hashlib.sha256(f.read_bytes()).hexdigest()!=digest:
  raise SystemExit('Official source mismatch: '+name)
key=dict(revision='lux-leaf-r1',archive_sha256=expected,patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest())
if out.exists():
 manifest=json.loads((out/'.lux-patch.json').read_text())
 if any(manifest.get(k)!=v for k,v in key.items()): raise SystemExit('Existing patch tree identity mismatch')
 for name,digest in manifest['files'].items():
  if hashlib.sha256((out/name).read_bytes()).hexdigest()!=digest: raise SystemExit('Modified patch output: '+name)
 print('VERIFIED_EXISTING_PATCH',key['patch_sha256']); raise SystemExit(0)
if out==o or out.is_relative_to(o): raise SystemExit('Patch output overlaps official source')
shutil.copytree(o,out)
subprocess.run(['git','apply','--check',str(patch)],cwd=out,check=True)
subprocess.run(['git','apply',str(patch)],cwd=out,check=True)
key['files']={str(f.relative_to(out)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(out.rglob('*')) if f.is_file()}
(out/'.lux-patch.json').write_text(json.dumps(key,indent=2))
print('APPLIED_VERIFIED_PATCH',key['patch_sha256'])
