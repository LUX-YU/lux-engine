"""Build the installation directory from explicitly registered module fragments."""
import argparse
import json
from pathlib import Path
from plugin_descriptor import write_if_changed
p = argparse.ArgumentParser()
p.add_argument('--output', required=True)
p.add_argument('--inputs', nargs='+', required=True)
p.add_argument('--base')
a = p.parse_args()
plugins = [json.loads(Path(f).read_text(encoding='utf-8')) for f in a.inputs]
if a.base:
    # An explicit consumer build replaces its own SDK example, preserving all other modules.
    own = {v['plugin']['id'] for v in plugins}
    base = json.loads(Path(a.base).read_text(encoding='utf-8'))
    plugins += [v for v in base['plugins'] if v['plugin']['id'] not in own]
plugins.sort(key=lambda v: v['plugin']['id'])
if len({v['plugin']['id'] for v in plugins}) != len(plugins):
    raise ValueError('duplicate plugin identity in installation')
write_if_changed(a.output, json.dumps(dict(format='lux.engine.capabilities', version=1, plugins=plugins), indent=2) + '\n')
