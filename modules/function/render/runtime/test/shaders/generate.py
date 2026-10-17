"""Regenerate the Camera fixture's complete matrix ABI (glslc + spirv-val, no custom compiler)."""
from pathlib import Path
import hashlib, os, struct, subprocess, tempfile
root = Path(__file__).resolve().parent
sdk = Path(os.environ['VULKAN_SDK']) / 'Bin'
source = root / 'Camera.frag'
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / 'Camera.frag.spv'
    subprocess.run([str(sdk / 'glslc.exe'), '--target-env=vulkan1.3', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(sdk / 'spirv-val.exe'), '--target-env', 'vulkan1.3', str(binary)], check=True)
    data = binary.read_bytes()
words = struct.unpack('<' + 'I' * (len(data) // 4), data)
lines = ['#pragma once', '#include <array>', '#include <cstdint>', 'namespace camera_fixture {',
    '// Source SHA256=' + hashlib.sha256(source.read_bytes()).hexdigest(),
    '// SPIR-V SHA256=' + hashlib.sha256(data).hexdigest(),
    f'inline constexpr std::array<std::uint32_t, {len(words)}> fragment = {{']
lines += ['    ' + ', '.join(f'0x{x:08x}' for x in words[i:i+8]) + ',' for i in range(0, len(words), 8)]
lines += ['};', '}']
(root / 'Camera.hpp').write_text('\n'.join(lines) + '\n')
