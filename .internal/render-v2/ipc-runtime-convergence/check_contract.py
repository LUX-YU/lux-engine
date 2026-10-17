"""Convergence-only source/deletion gate. Historical checkers are deliberately unchanged."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = a.source.resolve()
base = '9184c9f71c63af7f686822c76b89263b29ef7a63'
removed = [
    'modules/function/render/runtime/include/lux/engine/render/runtime/RenderClient.hpp',
    'modules/function/render/runtime/include/lux/engine/render/runtime/CommandValue.hpp',
    'modules/function/render/runtime/src/RenderClient.cpp',
    'modules/function/render/runtime/pinclude/TypedProtocol.hpp',
    'modules/platform/ipc/sinclude/lux/engine/platform/ipc/detail/MessageAccess.hpp',
]
assert all(not (root/path).exists() for path in removed)
files = []
for folder in ('modules/function/render', 'modules/platform/ipc'):
    for path in (root/folder).rglob('*'):
        if path.suffix not in ('.hpp', '.cpp', '.h') or 'test' in path.parts:
            continue
        if not any(part in path.parts for part in ('include', 'src', 'pinclude', 'sinclude')):
            continue
        files.append(path)
forbidden = re.compile(r'\b(?:RenderClient|RenderCompletion|TypedRenderCompletion|BoundCommand|IpcReply|'
    r'IpcBoundType|bindType|SessionIdentity|BudgetLease|ValueEnvelope|GetWorkCodePins|code_pin_|'
    r'implementation_code_pin|active_code_pin)\b')
violations = []
for path in files:
    for line, text in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if forbidden.search(text):
            violations.append((str(path.relative_to(root)), line, text))
assert not violations, violations
server = (root/'modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp').read_text()
assert 'vulkan::' not in server and 'CpuSchedule' not in server and 'TextureCommands' not in server
assert '/native/' not in server and 'DeviceOptions' not in server
assert 'static RenderResult<std::unique_ptr<RenderServer>> create' not in server
factory = (root/'modules/function/render/resource/include/lux/engine/render/resource/Factory.hpp').read_text()
for declaration in ('const StorageDescriptor* const description', 'const void* const ops', '(*const create)',
                    'const StorageCppType config_type', 'const StorageCppType native_type'):
    assert declaration in factory, declaration
work = (root/'modules/core/execution/include/lux/engine/core/execution/WorkSchedule.hpp').read_text()
assert 'GetWorkCodePins' not in work and 'shared_ptr' not in work
message = (root/'modules/platform/ipc/include/lux/engine/platform/ipc/IpcMessage.hpp').read_text()
assert message.index('private:') < message.index('struct TypeOps')
assert 'typed_inline_capacity = 80' in message
public = {}
for path in files + [root/'modules/core/execution/include/lux/engine/core/execution/WorkSchedule.hpp']:
    if 'include' in path.parts:
        public[str(path.relative_to(root)).replace('\\', '/')] = hashlib.sha256(path.read_bytes()).hexdigest()
head = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(dict(baseline=base, head=head, removed=removed,
    production_files=len(files), public_headers=public, result='PASS'), indent=2)+'\n')
print(f'PASS: {len(removed)} removed surfaces; {len(files)} production files; neutral Server; immutable Factory; no pin/ABI facade')
