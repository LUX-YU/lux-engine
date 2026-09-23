import sys, struct
from pathlib import Path
text = '#pragma once\n#include <cstdint>\nnamespace sample_ext {\n'
for name, file in zip(('kVertexShader', 'kFragmentShader'), sys.argv[2:]):
 data = Path(file).read_bytes()
 words = struct.unpack('<' + 'I' * (len(data)//4), data)
 text += 'inline constexpr std::uint32_t ' + name + '[]{' + ','.join(hex(w) for w in words) + '};\n'
text += '}\n'
p = Path(sys.argv[1])
if not p.exists() or p.read_text() != text: p.write_text(text)
