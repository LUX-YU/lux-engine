"""Serial, fail-closed stage runner. Invoke from the MSVC x64 developer environment."""
import datetime, json, os, subprocess, sys, time
from pathlib import Path

root = Path(sys.argv[1]).resolve()
label, slot, action = sys.argv[2:5]
build = Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w')/slot
out = root/label
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ)
env['PATH'] = ';'.join([str(build/'bin'), 'D:/Development/vcpkg/installed/x64-windows/bin',
                      *[p for p in env['PATH'].split(';') if 'CodeRepos' not in p and 'vcpkg' not in p]])

def run(name, command):
    path = out/(name+'.log')
    if path.exists(): raise RuntimeError('Refusing overwrite: '+str(path))
    start = time.monotonic()
    with path.open('wb') as log:
        log.write((json.dumps(command)+'\n').encode()); log.flush()
        code = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT, env=env)
    (out/(name+'.json')).write_text(json.dumps(dict(command=command, exit=code,
        utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), seconds=time.monotonic()-start), indent=2))
    print(name, code, round(time.monotonic()-start, 2), flush=True)
    if code: sys.exit(code)

if action == 'build':
    run(slot+'-all', ['cmake', '--build', str(build), '--target', 'all', '-j', '4', '--', '-k', '0'])
    run(slot+'-noop', ['cmake', '--build', str(build), '--target', 'all', '-j', '4', '--', '-k', '0'])
    if 'ninja: no work to do' not in (out/(slot+'-noop.log')).read_text():
        raise RuntimeError('Second build was not idle')
elif action == 'tests':
    run(slot+'-affected', ['ctest', '--test-dir', str(build), '--output-on-failure', '-j', '1',
        '-R', 'script|lua|flowforge', '--output-junit', str(out/(slot+'-ctest.xml'))])
elif action == 'alltests':
    run(slot+'-ctest', ['ctest', '--test-dir', str(build), '--output-on-failure', '-j', '1',
        '--output-junit', str(out/(slot+'-ctest.xml'))])
elif action == 'install':
    run(slot+'-install', ['cmake', '--install', str(build), '--config', 'RelWithDebInfo', '--component', 'lux_sdk'])
elif action == 'smoke':
    artifact = build.parent/'t/engine/toolchain/lua/lua_runtime_benchmark_fixture.lxsa'
    run(slot+'-oracle', [str(build/'bin/script_runtime_benchmark.exe'), '--group', 'scene-lua-event',
        '--mode', 'performance', '--size', '64', '--warmups', '5', '--frames', '20',
        '--seed', '1592598566', '--vm-accounting', 'off', '--lua-artifact', str(artifact),
        '--output', str(out/(slot+'-oracle.csv'))])
    text = (out/(slot+'-oracle.log')).read_text()
    if 'BUSINESS_ORACLE,lua-event,instances=64,cycles=25,completed=1600,per_instance=776,checksum=49664' not in text:
        raise RuntimeError('Missing business oracle')
else:
    raise RuntimeError(action)
