from pathlib import Path
import hashlib
import json
import os
import subprocess
import argparse
import shutil

parser = argparse.ArgumentParser(description='Verify installed scene-reader regeneration in an isolated SDK copy.')
parser.add_argument('--sdk', required=True)
parser.add_argument('--consumer', required=True)
parser.add_argument('--work-root', required=True)
parser.add_argument('--cmake', required=True)
parser.add_argument('--ninja', required=True)
parser.add_argument('--compiler', required=True)
parser.add_argument('--toolchain', required=True)
parser.add_argument('--prefix', action='append', required=True)
parser.add_argument('--commit', required=True)
args = parser.parse_args()
root = Path(args.work_root)
assert not root.exists(), 'Regeneration requires a new work directory'
root.mkdir(parents=True)
src, sdk, build = (root / name for name in ('src', 'sdk', 'build'))
shutil.copytree(args.consumer, src)
shutil.copytree(args.sdk, sdk)
logs = root / 'raw'
logs.mkdir()
cmake = args.cmake
ctest = str(Path(cmake).with_name('ctest.exe'))
os.environ['PATH'] = str(sdk / 'bin') + ';' + os.environ['PATH']
results = []


def run(label, command, expected=0, reason=None):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = logs / ('consumer-regeneration-' + label + '.log')
    log.write_bytes(result.stdout)
    text = result.stdout.decode('utf-8', errors='replace')
    okay = result.returncode == 0 if expected == 0 else result.returncode != 0
    okay = okay and (reason is None or reason in text)
    results.append(dict(step=label, exit=result.returncode, expected=expected, passed=okay, log=str(log)))
    (root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    assert okay, (label, result.returncode, text[-4000:])
    print(label, 'PASS', flush=True)
    return text


def configure(label):
    run(label, [cmake, '-S', str(src), '-B', str(build), '-G', 'Ninja',
                '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_MAKE_PROGRAM=' + args.ninja,
                '-DCMAKE_CXX_COMPILER=' + args.compiler, '-DCMAKE_TOOLCHAIN_FILE=' + args.toolchain,
                '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
                '-DCMAKE_PREFIX_PATH=' + ';'.join([str(sdk)] + args.prefix)])


def compile_step(label, **kwargs):
    return run(label, [cmake, '--build', str(build), '--target', 'all', '-j', '4', '--', '-k', '0'], **kwargs)


header = src / 'PluginComponent.hpp'
cmakelists = src / 'CMakeLists.txt'
template = sdk / 'share/lux-engine-editor-scene-ui/editor_scene_ui/cmake_scripts/codegen/inspector_codegen.py'
generated = build / ('inspector_gen/consumer_scene_readers/consumer__RichComponent_' +
                     hashlib.sha256(b'consumer::RichComponent').hexdigest()[:8] + '.inspector.generated.cpp')
original_header, original_cmake, original_template = (p.read_bytes() for p in (header, cmakelists, template))

configure('configure')
compile_step('baseline')
run('baseline-test', [ctest, '--test-dir', str(build), '-C', 'RelWithDebInfo', '--output-on-failure'])
initial = generated.read_bytes()
initial_mtime = generated.stat().st_mtime_ns
assert 'ninja: no work to do' in compile_step('baseline-noop')
assert generated.read_bytes() == initial and generated.stat().st_mtime_ns == initial_mtime

header.write_bytes(original_header.replace(b'display_name = Caption', b'display_name = CaptionChanged'))
compile_step('header')
assert b'CaptionChanged' in generated.read_bytes()

template.write_bytes(original_template.replace(b'Generated Editor-only ImGui implementation',
                                               b'ER2 generator dependency witness'))
compile_step('template')
assert b'ER2 generator dependency witness' in generated.read_bytes()

header.write_bytes(original_header.replace(
    b'        std::string LUX_MEMBER(display_name = Caption) caption{"Plugin component"};',
    b'#if defined(ER1_REGEN_LABEL)\n'
    b'        std::string LUX_MEMBER(display_name = MacroCaption) caption{"Plugin component"};\n'
    b'#else\n'
    b'        std::string LUX_MEMBER(display_name = DefaultCaption) caption{"Plugin component"};\n'
    b'#endif'))
compile_step('macro-off')
assert b'DefaultCaption' in generated.read_bytes() and b'MacroCaption' not in generated.read_bytes()
cmakelists.write_bytes(original_cmake + b'\ntarget_compile_definitions(scene_reader_plugin PRIVATE ER1_REGEN_LABEL)\n')
configure('macro-configure')
compile_step('macro-on')
assert b'MacroCaption' in generated.read_bytes() and b'DefaultCaption' not in generated.read_bytes()
assert 'ninja: no work to do' in compile_step('macro-noop')

good_output = generated.read_bytes()
good_mtime = generated.stat().st_mtime_ns
header.write_bytes(header.read_bytes().replace(b'display_name = Weight', b'display_name = Weight, widget = unapproved_probe'))
compile_step('invalid-widget', expected=1, reason='unknown widget unapproved_probe')
assert generated.read_bytes() == good_output and generated.stat().st_mtime_ns == good_mtime
print('validation failure preserves last complete output PASS', flush=True)

header.write_bytes(original_header)
cmakelists.write_bytes(original_cmake)
template.write_bytes(original_template)
configure('restore-configure')
compile_step('restore')
assert generated.read_bytes() == initial
run('restored-test', [ctest, '--test-dir', str(build), '-C', 'RelWithDebInfo', '--output-on-failure'])
assert 'ninja: no work to do' in compile_step('restored-noop')
results.append(dict(step='output-identity', sha256=hashlib.sha256(initial).hexdigest(),
                    restored_sha256=hashlib.sha256(generated.read_bytes()).hexdigest(), passed=True))
(root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
(root / 'identity.json').write_text(json.dumps(dict(source_commit=args.commit, input_sdk=args.sdk,
    input_consumer=args.consumer, compiler=args.compiler, configuration='RelWithDebInfo',
    sdk_copy=str(sdk), generated_sha256=hashlib.sha256(initial).hexdigest()), indent=2), encoding='utf-8')
print('All installed SDK regeneration checks passed; source commit ' + args.commit, flush=True)

