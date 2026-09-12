"""Keep the application's integrity output separate from VTune's report stream."""
import json, subprocess, sys
from pathlib import Path
output = Path(sys.argv[1])
with output.open('wb') as log:
    result = subprocess.run(sys.argv[2:], stdout=log, stderr=subprocess.STDOUT)
output.with_suffix('.exit.json').write_text(json.dumps({'application_exit':result.returncode}))
sys.exit(result.returncode)
