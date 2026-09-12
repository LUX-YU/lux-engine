"""Preserve runnable stage images before a controlled build slot is reused."""
import hashlib, json, shutil, subprocess, sys
from pathlib import Path

root, label = Path(sys.argv[1]).resolve(), sys.argv[2]
for slot in sys.argv[3:]:
    build = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w')/slot
    target = root/'images'/label/slot
    target.mkdir(parents=True, exist_ok=False)
    source = next(line.split('=', 1)[1] for line in (build/'CMakeCache.txt').read_text().splitlines()
                  if line.startswith('CMAKE_HOME_DIRECTORY:'))
    assert not subprocess.check_output(['git','-C',source,'status','--porcelain'],text=True).strip()
    commit = subprocess.check_output(['git','-C',source,'rev-parse','HEAD'],text=True).strip()
    images = {}
    for path in sorted((build/'bin').iterdir()):
        keep = path.suffix == '.dll' or (path.suffix in ['.exe','.pdb'] and
            any(name in path.name for name in ['script_runtime_benchmark','physics2d_script_benchmark','script_lua','script_core',
                'simulation_script','function_script','lua51','lua54','lux_lua55']))
        if not path.is_file() or not keep: continue
        saved = target/path.name
        shutil.copy2(path,saved)
        with saved.open('rb') as stream: digest = hashlib.file_digest(stream,'sha256').hexdigest()
        images[path.name] = dict(bytes=saved.stat().st_size,sha256=digest)
    for name in ['CMakeCache.txt','compile_commands.json']:
        shutil.copy2(build/name,target/name)
    artifact = build.parent/'t/engine/toolchain/lua/lua_runtime_benchmark_fixture.lxsa'
    shutil.copy2(artifact,target/artifact.name)
    with artifact.open('rb') as stream: digest = hashlib.file_digest(stream,'sha256').hexdigest()
    fixtures = {}
    for name in ['physics2d_lua_fixture.lxsa', 'physics2d_flowforge_fixture.lxsa']:
        fixture = build.parent/'t/engine/toolchain/physics2d'/name
        shutil.copy2(fixture, target/name)
        fixtures[name] = hashlib.sha256(fixture.read_bytes()).hexdigest()
    (target/'identity.json').write_text(json.dumps(dict(source=source,commit=commit,slot=slot,
        artifact_sha256=digest,physics_fixtures=fixtures,images=images),indent=2))
    print(label,slot,commit,len(images),flush=True)
