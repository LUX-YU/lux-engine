"""Archive this investigation once; preserve invalid attempts and exclude compiled images."""
import hashlib, json, sys, zipfile
from pathlib import Path

root=Path(sys.argv[1]).resolve()
destination=Path(sys.argv[2]).resolve()
destination.mkdir(parents=True,exist_ok=True)
archive=destination/'lua-event-phases-raw.zip'
assert not archive.exists(), 'Refuse to overwrite an existing evidence archive'
disposition={
    'compile.log':'INVALID: missing iostream; corrected compile-2.log passed; production unchanged',
    'hardware':'INVALID: VTune sampling driver/admin requirements unavailable',
    'roi-0':'EXCLUDED: no captured application integrity output; CSV alone was insufficient',
    'roi-1':'VALID: 50M timed new calls/resumes; independent application exit and integrity',
    'roi-2':'VALID: 50M timed new calls/resumes; independent application exit and integrity',
    'timing-0..4':'VALID: five original/probe observation-control pairs, not production speedups',
    'memory-0..1':'VALID: VM accounting only; their durations are not performance evidence',
    'wrapper-only-attribution':'NOT USED: tail calls remove wrappers; use ITT task-filtered reports',
    'hardware_precision':'NOT AVAILABLE: software CPU samples do not prove stalls/cache/branch events'
}
(root/'trial-disposition.json').write_text(json.dumps(disposition,indent=2))
excluded_extensions={'.exe','.dll','.pdb','.obj','.lib','.ilk','.exp'}
files=[p for p in sorted(root.rglob('*')) if p.is_file()
       and 'images' not in p.relative_to(root).parts and p.suffix.lower() not in excluded_extensions]
manifest=[{'path':p.relative_to(root).as_posix(),'bytes':p.stat().st_size,
           'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in files]
with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=7) as z:
    for p,item in zip(files,manifest):z.write(p,item['path'])
    z.writestr('raw-files.json',json.dumps(manifest,indent=2))
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    for item in manifest:
        assert hashlib.sha256(z.read(item['path'])).hexdigest()==item['sha256']
sha=hashlib.sha256(archive.read_bytes()).hexdigest()
(destination/'SHA256SUMS').write_text(sha+'  '+archive.name+'\n')
(destination/'archive-summary.json').write_text(json.dumps({
    'archive':archive.name,'sha256':sha,'archive_bytes':archive.stat().st_size,
    'files':len(manifest),'raw_bytes':sum(i['bytes'] for i in manifest),
    'compiled_images_in_archive':False,'raw_vtune_projects':['roi-0 (excluded)','roi-1','roi-2'],
    'all_file_hashes_verified':True},indent=2))
print('ARCHIVE VERIFIED',len(manifest),'files',archive.stat().st_size,'bytes',sha)
