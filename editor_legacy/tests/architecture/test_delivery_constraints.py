"""Compile the actual private delivery header with actual Material UI compilation flags."""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
args = parser.parse_args()
entries = json.loads((args.build / 'compile_commands.json').read_text())
entry = next(x for x in entries if x['file'].replace('\\', '/').endswith('editor_legacy/workbench/material/src/MaterialView.cpp'))
folder = args.build / 'delivery-constraints'
folder.mkdir(exist_ok=True)
prefix = '''#include <lux/engine/editor/workbench/InteractionDelivery.hpp>
using namespace lux::editor::workbench::detail;
enum class EError { BUSY };
enum class EOther { STALE };
using Result = lux::cxx::expected<void, EError>;
struct Action { Result operator()() & { return {}; } };
struct RvalueOnly { Result operator()() && { return {}; } };
'''
cases = {
    'positive': 'Action cancel; Action preview; auto validate = []() -> Result { return {}; };',
    'bool_return': 'auto cancel = [] { return true; }; Action preview; auto validate = []() -> Result { return {}; };',
    'different_error': 'Action cancel; auto preview = []() -> lux::cxx::expected<void, EOther> { return {}; }; auto validate = []() -> Result { return {}; };',
    'rvalue_only': 'RvalueOnly cancel; Action preview; auto validate = []() -> Result { return {}; };',
    'nonvoid_result': 'Action cancel; Action preview; auto validate = []() -> lux::cxx::expected<int, EError> { return 1; };',
}
results = []
for name, body in cases.items():
    source = folder / (name + '.cpp')
    source.write_text(prefix + 'void check() { ' + body + '''
        Action begin, finish;
        EInputDeliveryStage stage = EInputDeliveryStage::BEGIN;
        auto result = deliverInput(stage, true, validate, cancel, begin, preview, finish);
    }
    ''')
    command = entry['command']
    command = command.replace(entry['file'].replace('/', '\\'), source.as_posix()).replace(entry['file'], source.as_posix())
    command = re.sub(r'/Fo(?:"[^"]+"|\S+)', '/Fo"' + (folder / (name + '.obj')).as_posix() + '"', command)
    result = subprocess.run(command, cwd=entry['directory'], capture_output=True, text=True)
    log = result.stdout + result.stderr
    (folder / (name + '.log')).write_text(log, encoding='utf-8')
    if name == 'positive':
        assert result.returncode == 0, log
    else:
        assert result.returncode != 0 and 'C1083' not in log, log
        assert 'deliverInput' in log and any(x in log for x in ('C7602', 'constraints', 'DeliveryAction', 'VoidDeliveryResult')), log
    results.append({'case': name, 'exit_code': result.returncode, 'log': name + '.log'})
    print('PASS', name, result.returncode)
(folder / 'results.json').write_text(json.dumps(results, indent=2))
