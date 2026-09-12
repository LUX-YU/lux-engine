"""Closing identity audit. Run after serial measurement completes, never concurrently with timing."""
import hashlib,json,subprocess,sys
from pathlib import Path
root=Path(sys.argv[1]).resolve()
source=Path(__file__).resolve().parents[3]
initial=json.loads((root/'baseline-start.json').read_text())
records=[]
for entry in initial['repositories']:
    path=Path(entry['path'])
    if path.name not in ['lux-engine','lux-cxx','lux-cmake-toolset']: continue
    head=subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'],text=True).strip()
    status=subprocess.check_output(['git','-C',str(path),'status','--porcelain=v1','-uall'],text=True).strip()
    unchanged=[]
    for file in entry['files']:
        actual=hashlib.sha256(Path(file['path']).read_bytes()).hexdigest()
        unchanged.append(dict(path=file['path'],sha256=actual,matches_initial=actual==file['sha256']))
    assert head==entry['head'] and status==entry['status'].strip(),str(path)
    assert all(f['matches_initial'] for f in unchanged)
    records.append(dict(path=str(path),head=head,status=status,files=unchanged,
        limit='First porcelain line hash absent in initial capture; not fabricated retroactively'))
image_checks=[]
for label,slot in [('final-lua55','d'),('final-lua54','l'),('final-jit','d')]:
    path=root/'images'/label/slot
    identity=json.loads((path/'identity.json').read_text())
    for name,record in identity['images'].items():
        with (path/name).open('rb') as stream: actual=hashlib.file_digest(stream,'sha256').hexdigest()
        assert actual==record['sha256'],name
    commands=json.loads((path/'compile_commands.json').read_text())
    selected=[c for c in commands if any(n in c['file'] for n in [
        'lua_value_test.cpp','lua_closure_provenance_test.cpp','lua_coroutine_integration_test.cpp',
        'script_system_event_wait_test.cpp','script_system_lifecycle_test.cpp'])]
    assert len(selected)>=5
    assert all('/UNDEBUG' in c['command'] or '-UNDEBUG' in c['command'] for c in selected)
    image_checks.append(dict(label=label,source=identity['commit'],verified_images=len(identity['images']),
        assertion_commands=[dict(file=c['file'],command=c['command']) for c in selected]))
sdk_files=[]
sdk=root/'relocated/sdk'
assert not list(sdk.rglob('LuaVmCompatibility.hpp'))
assert not any(p.name in ['pinclude','sinclude'] for p in sdk.rglob('*') if p.is_dir())
for prefix in [sdk,root/'relocated/tools',root/'relocated/vm']:
    for file in sorted(prefix.rglob('*')):
        if not file.is_file(): continue
        if file.suffix not in ['.dll','.exe','.lib','.hpp','.h','.cmake','.template','.in']: continue
        with file.open('rb') as stream: digest=hashlib.file_digest(stream,'sha256').hexdigest()
        sdk_files.append(dict(path=file.relative_to(root).as_posix(),bytes=file.stat().st_size,sha256=digest))
(root/'installed-files.json').write_text(json.dumps(sdk_files,indent=2))
for profile_label in ['profile55','profile55-captured']:
    profile=root/profile_label/'result'
    if not profile.exists(): continue
    profile_files=[]
    for file in sorted(profile.rglob('*')):
        if file.is_file():
            with file.open('rb') as stream: digest=hashlib.file_digest(stream,'sha256').hexdigest()
            profile_files.append(dict(path=file.relative_to(root).as_posix(),bytes=file.stat().st_size,sha256=digest))
    (root/profile_label/'raw-vtune-files.json').write_text(json.dumps(profile_files,indent=2))
(root/'closing-audit.json').write_text(json.dumps(dict(repositories=records,images=image_checks,
    installed_file_count=len(sdk_files),private_headers_absent=True),indent=2))
print('AUDIT',len(records),'protected repositories;',len(image_checks),'image sets;',len(sdk_files),'installed files')
