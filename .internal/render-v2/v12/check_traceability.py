"""Validate v12 work allocation, not implementation correctness."""
import json
from pathlib import Path

root = Path(__file__).resolve().parent
def read(name):
    return json.loads((root / name).read_text(encoding='utf-8'))

work = read('work-packages.json')
assert len(work) == 38
for file, count in [('requirements.json', 86), ('member-dispositions.json', 229), ('deletion-map.json', 34)]:
    rows = read(file)
    assert len(rows) == count, file
    assert len({row['id'] for row in rows}) == count, file
    for row in rows:
        assert row['work_package'] in work, row['id']
        assert 'evidence' in row, row['id']
        if row.get('status') in ('PASS', 'FIXED', 'PROTECTED'):
            assert row['evidence'], row['id']
for key, value in work.items():
    assert value['production_paths'] and value['test_requirements'], key
sources = read('algorithm-sources-corrected.json')
assert not any(row['path'].startswith('editor_legacy/') for row in sources['sources'])
print('PASS: 38 work packages; 86 requirements; 229 audit groups; 34 deletion rows. Statuses are not qualification.')
