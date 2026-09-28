from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[2]
out = root / '.internal/editor-redesign/P03-R1-before'
build = root.parent / 'build/RelWithDebInfo/lux-engine'
records = []
for scenario in ['reload-unbound', 'reload-unbound-lease', 'reload-closing-lease',
                 'reload-reading-lease', 'reload-outcomes']:
    args = [str(build / 'bin/editor_material_model_test.exe'), scenario]
    result = subprocess.run(args, cwd=build / 'bin', stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = out / (scenario + '.log')
    log.write_bytes(result.stdout)
    records.append({'scenario': scenario, 'argv': args, 'cwd': str(build / 'bin'),
                    'exit_code': result.returncode, 'target_contract_passed': result.returncode == 0,
                    'archive_log': 'dev_log/P03-R1/before/' + log.name,
                    'sha256': hashlib.sha256(result.stdout).hexdigest()})
    print(scenario, 'exit', result.returncode, flush=True)
    print(result.stdout.decode(errors='replace'), flush=True)
(out / 'results.json').write_text(json.dumps(records, indent=2) + '\n')
assert records[0]['exit_code'] != 0, 'The original real-model reentry defect must be reproduced.'
assert records[-1]['exit_code'] == 0, 'Existing identity/clone/success behavior must still pass.'
