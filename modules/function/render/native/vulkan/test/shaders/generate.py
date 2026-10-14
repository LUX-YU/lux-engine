"""Rebuild legal test SPIR-V; not a runtime Shader parser or material authoring system."""
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import os
import sys

root = Path(__file__).resolve().parent
sdk = Path(os.environ['VULKAN_SDK']) / 'Bin'
swizzle = '--swizzle' in sys.argv
output = ['#pragma once', '#include <array>', '#include <cstdint>', 'namespace native_fixture {']
with tempfile.TemporaryDirectory() as temporary:
    for stage in (['frag'] if swizzle else ['vert', 'frag']):
        source = root / (('Swizzle.' if swizzle else 'Triangle.') + stage)
        binary = Path(temporary) / (stage + '.spv')
        subprocess.run([str(sdk / 'glslc.exe'), '--target-env=vulkan1.3', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(sdk / 'spirv-val.exe'), '--target-env', 'vulkan1.3', str(binary)], check=True)
        data = binary.read_bytes()
        output.append('// source sha256=' + hashlib.sha256(source.read_bytes()).hexdigest())
        output.append('// SPIR-V sha256=' + hashlib.sha256(data).hexdigest())
        words = struct.unpack('<' + 'I' * (len(data) // 4), data)
        output.append(f'inline constexpr std::array<std::uint32_t, {len(words)}> {'swizzle' if swizzle else stage} = {{')
        for start in range(0, len(words), 8):
            output.append('    ' + ', '.join(f'0x{x:08x}' for x in words[start:start + 8]) + ',')
        output.append('};')
output.append('}')
(root / ('Swizzle.hpp' if swizzle else 'Triangle.hpp')).write_text('\n'.join(output) + '\n', encoding='utf-8')
