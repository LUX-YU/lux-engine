"""Preserve the profiled child's stdout/stderr independently of VTune's Windows console handling."""
import json, subprocess, sys
from pathlib import Path
out=Path(sys.argv[1]).resolve()
command=json.loads((out/'target-command.json').read_text())
with (out/'target.log').open('wb') as log:
    process=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
    code=process.wait()
(out/'target-exit.json').write_text(json.dumps(dict(command=command,pid=process.pid,exit=code),indent=2))
sys.exit(code)
