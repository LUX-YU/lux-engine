"""Drive generated Run controls in the actual installed real-views fixture; no private API."""
from pathlib import Path
import difflib, hashlib, json, subprocess, sys
w=Path(__file__).resolve().parent
c=json.loads((w/'final-config.json').read_text())
s=Path(c['source']); sdk=Path(c['prefix'])
b=s.parent/'build/RelWithDebInfo'/('ec3-sdk-'+c['implementation_sha'][:12])/'desktop-views'
target=b/'views.cpp'
original=target.read_text()
expected=(s/'editor/tests/integration/scene_views/views.cpp').read_text().replace(
    '../../../../../cmake/installed-consumers/common/ControlsTestAccess.hpp', 'ControlsTestAccess.hpp')
assert original==expected
anchor='        assert(run_fields_view->target() == target);\n'
assert original.count(anchor)==1
changed=original.replace(anchor,anchor+(w/'run-controls.fragment.cpp').read_text())
target.write_text(changed)
(w/'run-controls.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),changed.splitlines(True),
    fromfile='views.cpp@'+c['implementation_sha'],tofile='SDK-generated-control-evidence/views.cpp')))
(w/'run-controls-source.json').write_text(json.dumps(dict(source=str(target),implementation_sha=c['implementation_sha'],
    original_sha256=hashlib.sha256(original.encode()).hexdigest(),fixture_sha256=hashlib.sha256(changed.encode()).hexdigest()),indent=2)+'\n')
def run(label,args):
    subprocess.run([sys.executable,str(w/'run.py'),'--source',str(s),'--cwd',str(s),'--runtime',str(sdk/'bin'),label,*map(str,args)],check=True)
try:
    run('final-sdk-run-controls-build',['cmake','--build',b,'--target','all','-j','4','--','-k','0'])
    run('final-sdk-run-controls-test',['ctest','--test-dir',b,'--output-on-failure','-R','^installed.desktop.real_views$'])
    (w/'final-sdk-run-controls-details.log').write_bytes((b/'Testing/Temporary/LastTest.log').read_bytes())
finally:
    target.write_text(original)
