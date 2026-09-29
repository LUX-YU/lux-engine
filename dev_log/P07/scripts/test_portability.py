"""Probe P07 archive gate without hiding or changing producer/user files."""
from pathlib import Path
import argparse,importlib.util,json,unittest.mock,contextlib,io,sys
sys.dont_write_bytecode=True
parser=argparse.ArgumentParser()
parser.add_argument('--repo',type=Path)
parser.add_argument('--output',type=Path)
args=parser.parse_args()
root=args.repo.resolve() if args.repo else next(p for p in Path(__file__).resolve().parents if (p/'dev_log/P07/check_receipt.py').is_file())
spec=importlib.util.spec_from_file_location('p07_gate',root/'dev_log/P07/check_receipt.py')
gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
original_open=Path.open; original_bytes=Path.read_bytes
victim=(root/'dev_log/P07/logs/compilation-detail.log').resolve()
results=[]
for mode in ['producer-unavailable','missing-result','corrupt-result']:
 def guarded_open(path,*args,**kwargs):
  resolved=path.resolve()
  if not resolved.is_relative_to(root/'dev_log'):
   raise FileNotFoundError('Producer files unavailable: '+str(path))
  return original_open(path,*args,**kwargs)
 def guarded_bytes(path):
  if path.resolve()==victim:
   if mode=='missing-result':raise FileNotFoundError(victim)
   if mode=='corrupt-result':return b'corrupt archive'
  return original_bytes(path)
 success=False;failure=''
 with unittest.mock.patch.object(Path,'open',guarded_open),unittest.mock.patch.object(Path,'read_bytes',guarded_bytes),contextlib.redirect_stdout(io.StringIO()):
  try:gate.check(root);success=True
  except (AssertionError,FileNotFoundError) as error:failure=type(error).__name__
 expected=mode=='producer-unavailable';results.append(dict(case=mode,accepted=success,expected=expected,matched=success==expected,failure=failure))
 print(mode,'PASS' if success==expected else 'FAIL')
assert all(r['matched'] for r in results)
if args.output:args.output.write_text(json.dumps(results,indent=2)+'\n')
