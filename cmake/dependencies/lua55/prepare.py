"""Verify a separately downloaded official archive; never replace an existing source tree."""
import argparse
import hashlib
import json
import tarfile
from pathlib import Path

EXPECTED = '1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce'
parser = argparse.ArgumentParser()
parser.add_argument('--archive', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
arguments = parser.parse_args()
archive = arguments.archive.resolve(strict=True)
output = arguments.output.resolve()
with archive.open('rb') as stream:
    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
if digest != EXPECTED:
    raise SystemExit('Official Lua 5.5.1 archive SHA-256 mismatch; nothing extracted')
if output.exists():
    raise SystemExit('Output must not exist; existing source is never replaced')
with tarfile.open(archive, 'r:gz') as source:
    members = []
    for entry in source.getmembers():
        path = Path(entry.name)
        if path.parts[0] != 'lua-5.5.1' or not (entry.isfile() or entry.isdir()):
            raise SystemExit('Unexpected archive member')
        target = (output/Path(*path.parts[1:])).resolve()
        if not target.is_relative_to(output):
            raise SystemExit('Archive member escapes output')
        members.append((entry, target))
    output.mkdir(parents=True)
    for entry, target in members:
        if entry.isdir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.extractfile(entry).read())
files = {str(path.relative_to(output)): hashlib.sha256(path.read_bytes()).hexdigest()
         for path in sorted(output.rglob('*')) if path.is_file()}
(output.parent/(output.name+'.identity.json')).write_text(json.dumps(dict(
    archive=str(archive), archive_sha256=digest, files=files), indent=2))
print('VERIFIED_LUA55', output, len(files), digest)
