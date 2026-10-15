"""Cook the test consumer with the installed Vulkan SDK compiler; output carries source/SPIR-V hashes."""
import hashlib
import pathlib
import struct
import subprocess
import sys

root = pathlib.Path(__file__).parent
compiler, output_dir = sys.argv[1], pathlib.Path(sys.argv[2])
output_dir.mkdir(parents=True, exist_ok=True)
lines = ['#pragma once', '#include <array>', '#include <cstdint>', 'namespace texture_fixture {']
for stage, symbol in [('vert', 'vert'), ('frag', 'frag')]:
    source = root / ('texture.' + stage)
    binary = output_dir / ('texture.' + stage + '.spv')
    subprocess.run([compiler, '-V', '--target-env', 'vulkan1.3', str(source), '-o', str(binary)], check=True)
    data = binary.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    lines += ['// source sha256=' + hashlib.sha256(source.read_bytes().replace(b'\r\n', b'\n')).hexdigest(),
              '// SPIR-V sha256=' + hashlib.sha256(data).hexdigest(),
              f'inline constexpr std::array<std::uint32_t, {len(words)}> {symbol} = {{']
    lines += ['    ' + ', '.join(f'0x{word:08x}' for word in words[i:i+8]) + ',' for i in range(0, len(words), 8)]
    lines += ['};']
lines += ['}']
(root / 'Texture.hpp').write_text('\n'.join(lines) + '\n')
