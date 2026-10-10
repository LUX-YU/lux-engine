"""Exercise the production emitter/compiler/validator, retaining every raw artifact."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

glslc, spirv_val, complex_tool, tonemap_tool, stage_tool, sources, output = sys.argv[1:]
sources, output = Path(sources), Path(output)
output.mkdir(parents=True, exist_ok=True)
records = []

def run(name, args, success=True, diagnostic=None):
    result = subprocess.run([str(x) for x in args], capture_output=True)
    (output / (name + '.log')).write_bytes(result.stdout + result.stderr)
    records.append({'name': name, 'command': [str(x) for x in args], 'exit_code': result.returncode})
    assert (result.returncode == 0) == success, (name, result.stdout, result.stderr)
    if diagnostic:
        assert diagnostic in (result.stdout + result.stderr).decode(errors='replace'), name

def chain(name, tool, source, stage, fail_at=None, diagnostic=None):
    generated, binary = output / (name + '.glsl'), output / (name + '.spv')
    run(name + '-emit', [tool, source, generated], fail_at != 'emit', diagnostic if fail_at == 'emit' else None)
    if fail_at == 'emit': return
    run(name + '-compile', [glslc, '-fshader-stage=' + stage, '--target-env=vulkan1.2', '-I' + str(sources),
                            generated, '-o', binary], fail_at != 'compile', diagnostic if fail_at == 'compile' else None)
    if fail_at == 'compile': return
    run(name + '-spirv-val', [spirv_val, '--target-env', 'vulkan1.2', binary])
    run(name + '-reflect', [tool, binary, 'validate'], fail_at != 'reflect')

for name, tool, stage in [('F3Complex.comp', complex_tool, 'compute'), ('F3Tonemap.frag', tonemap_tool, 'fragment'),
                           ('LegacyTonemap.vert', stage_tool, 'vertex'), ('Stages.vert', stage_tool, 'vertex'),
                           ('Stages.frag', stage_tool, 'fragment')]:
    chain(name, tool, sources / (name + '.lglsl'), stage)

base = (sources / 'F3Tonemap.frag.lglsl').read_text()
negative = {
    'duplicate-version': (base.replace('#version 450', '#version 450\n#version 450'), 'emit', 'Duplicate or late #version'),
    'late-version': (base.replace('#version 450', '#define BEFORE_VERSION 1\n#version 450'), 'emit', 'Duplicate or late #version'),
    'late-extension': (base.replace('//! lux-pass-declarations', '//! lux-pass-declarations\n#extension GL_EXT_debug_printf : enable'), 'emit', 'Late #extension'),
    'duplicate-marker': (base.replace('//! lux-pass-declarations', '//! lux-pass-declarations\n//! lux-pass-declarations'), 'emit', 'Duplicate lux-pass-declarations'),
    'wrong-stage': (base.replace('stage=fragment', 'stage=banana'), 'emit', 'stage'),
    'manual-binding': (base + '\nlayout(set=2,binding=0) uniform texture2D escape;\n', 'emit', 'physical set/binding'),
    'helper-before-declarations': (base.replace('//! lux-pass-declarations\n#include "F3TonemapHelper.lglslh"', '#include "F3TonemapHelper.lglslh"\n//! lux-pass-declarations'), 'compile', 'undeclared'),
    'undefined-helper-field': (base.replace('color = f3Tonemap', 'color = missing_field + f3Tonemap'), 'compile', 'undeclared'),
    'wrong-compile-stage': (base, 'compile', None),
}
for name, (source, failure, diagnostic) in negative.items():
    source_path = output / (name + '.lglsl')
    source_path.write_text(source, encoding='utf-8')
    chain(name, tonemap_tool, source_path, 'compute' if name == 'wrong-compile-stage' else 'fragment', failure, diagnostic)

(output / 'commands.json').write_text(json.dumps(records, indent=2))
hashes = {str(p.relative_to(output)): hashlib.sha256(p.read_bytes()).hexdigest()
          for p in output.iterdir() if p.is_file() and p.name != 'hashes.json'}
hashes.update({'input/' + p.name: hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sources.iterdir() if p.suffix in ('.lglsl', '.lglslh', '.hpp')})
(output / 'hashes.json').write_text(json.dumps(hashes, indent=2))
print('F3 include contract: production emitter, glslc, spirv-val and reflection PASS')
