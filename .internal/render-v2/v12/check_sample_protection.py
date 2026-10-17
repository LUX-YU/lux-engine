"""Preserve the pre-existing protection checks and the newly observed user header edit."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
subprocess.run(['py', '-3', str(args.source / '.internal/render-v2/v12/check_material_protection.py'),
    '--source', str(args.source), '--output', str(args.output)], check=True)
path = Path('E:/RenderV2Rebuild/V12/source/modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp')
expected = '80a98f9e08ff0eb18b5f6ae0b3dea862f59384b71d9d36efe9c041acbec957df'
assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, 'Newly protected user header changed'
result = json.loads(args.output.read_text())
result['additional_user_file'] = {'path': str(path), 'sha256': expected}
args.output.write_text(json.dumps(result, indent=2) + '\n')
print('PASS: original 719 blobs/three user files plus new isolated-tree RenderServer header protection')
