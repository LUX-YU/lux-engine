"""Read-only fixture sanity. Requires Python 3.11+. Does NOT run Lux or emulate its importer."""
from pathlib import Path
import hashlib
import json
import tomllib

def main():
    root = Path(__file__).resolve().parents[1]
    results = []
    for case in sorted((root / 'fixtures').iterdir()):
        if not case.is_dir():
            continue
        expected = json.loads((case / 'EXPECTATIONS.json').read_text(encoding='utf-8'))
        settings = tomllib.loads((case / '.lux/editor/settings.toml').read_text(encoding='utf-8'))
        assert settings['version'] == 1 and settings['selected'] == expected['selected_name']
        files = sorted((case / '.lux/editor/layouts').glob('*.toml'))
        assert len(files) == expected['expected_layout_count']
        parsed = []
        for file in files:
            data = file.read_bytes()
            value = tomllib.loads(data.decode('utf-8'))
            assert value['version'] == 1 and isinstance(value['dock'], str) and value['dock']
            panes = value['panes']
            assert len(panes) == len({p['id'] for p in panes})
            for pane in panes:
                assert pane['type'] == 'lux.editor.material.v1'
                assert len(pane['payload']) == 39 and pane['payload'].startswith('v1:')
            parsed.append({'filename': file.name, 'sha256': hashlib.sha256(data).hexdigest()})
        results.append({'case': case.name, 'fixture_parse': 'PASS', 'files': parsed, 'lux_executed': False})
    print(json.dumps({'scope': 'TOML parsing and input consistency only; not a C++/SDK migration test',
                      'cases': results}, ensure_ascii=False, indent=2))

if __name__ == '__main__':
    main()
