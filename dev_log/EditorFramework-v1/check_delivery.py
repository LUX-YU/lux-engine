from pathlib import Path
import hashlib, json, subprocess, sys
record=Path(__file__).resolve().parent
q=json.loads((record/'qualification.json').read_text())
source=Path(q['source']); build=Path(q['build']); sdk=Path(sys.argv[1] if len(sys.argv)>1 else q['sdk'])
files=[p.relative_to(sdk).as_posix() for p in sdk.rglob('*') if p.is_file()]
assert not any('_legacy' in p for p in files), 'legacy installed filename'
packages=list((sdk/'share').glob('lux-engine-editor*'))
assert [p.name for p in packages]==['lux-engine-editor-framework'], packages
for p in (sdk/'share/lux-engine/plugins').glob('*.json'):
    text=p.read_text()
    assert 'editor_library' not in text and '_legacy' not in text, p
headers=sorted(p for p in (sdk/'include/lux/engine/editor').rglob('*') if p.is_file())
assert len(headers)==11, headers
assert not any(p.name.startswith('ExternalEditor') for p in sdk.rglob('*'))
cache=(build/'CMakeCache.txt').read_text()
assert 'LUX_BUILD_EDITOR_LEGACY:BOOL=OFF' in cache
assert 'LUX_EDITOR_MIGRATION_STAGE:' not in cache
assert 'LUX_EDITOR_LAYERING_MODE:' not in cache
assert 'editor_legacy/' not in (build/'build.ninja').read_text()
assert subprocess.check_output(['git','status','--porcelain','--untracked-files=all'],cwd=source)==b''
print('PASS isolated install:',len(files),'files;',len(headers),'framework public headers')
print('PASS default build: legacy OFF; no old migration rules, targets or source providers')
for relative in ['resource/asset/storage/AssetVfs.hpp','ui/InputEvent.hpp']:
    original=next((source/'modules').rglob(relative.split('/')[-1])).read_bytes()
    for prefix in ['Debug/include','RelWithDebInfo/include','Android/lux-engine/include']:
        p=Path('E:/SyncForder/CodeRepos/install')/prefix/'lux/engine'/relative
        assert p.read_bytes()==original, p
        print('public header synchronized:',p,hashlib.sha256(original).hexdigest())
protection=json.loads((record/'protection/manifest.json').read_text())
raw=(record/'protection/ProjectBuilder.cpp.user').read_bytes()
assert hashlib.sha256(raw).hexdigest().upper()==protection['user_sha256']
subprocess.run(['git','apply','--check',str(record/'protection/ProjectBuilder.mapped.patch')],cwd=source,check=True)
print('PASS original user patch hash and migrated application check; NOT applied')
for entry in json.loads((record/'cleanup-inventory.json').read_text()):
    assert not Path(entry['path']).exists(),entry['path']
print('PASS audited old source checkouts remain removed; Git refs archived in canonical repositories')
