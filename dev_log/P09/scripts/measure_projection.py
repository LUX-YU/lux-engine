"""Measure the real P07 projection executable; no production instrumentation or test replacement."""
import ctypes,ctypes.wintypes as wt,json,subprocess,time,argparse
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--program',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
class Memory(ctypes.Structure):
 _fields_=[('cb',wt.DWORD),('PageFaultCount',wt.DWORD),('PeakWorkingSetSize',ctypes.c_size_t),('WorkingSetSize',ctypes.c_size_t),('QuotaPeakPagedPoolUsage',ctypes.c_size_t),('QuotaPagedPoolUsage',ctypes.c_size_t),('QuotaPeakNonPagedPoolUsage',ctypes.c_size_t),('QuotaNonPagedPoolUsage',ctypes.c_size_t),('PagefileUsage',ctypes.c_size_t),('PeakPagefileUsage',ctypes.c_size_t),('PrivateUsage',ctypes.c_size_t)]
get_memory=ctypes.WinDLL('psapi').GetProcessMemoryInfo
get_memory.argtypes=[wt.HANDLE,ctypes.POINTER(Memory),wt.DWORD];get_memory.restype=wt.BOOL
runs=[]
for iteration in range(5):
 stdout_path=args.output.parent/f'projection-measure-{iteration}.log'
 peak_working=peak_private=samples=0
 start=time.perf_counter_ns()
 with stdout_path.open('wb') as log:
  process=subprocess.Popen([str(args.program)],cwd=args.program.parent,stdout=log,stderr=subprocess.STDOUT)
  while process.poll() is None:
   counters=Memory();counters.cb=ctypes.sizeof(counters)
   if get_memory(wt.HANDLE(int(process._handle)),ctypes.byref(counters),counters.cb):
    samples+=1;peak_working=max(peak_working,counters.PeakWorkingSetSize);peak_private=max(peak_private,counters.PrivateUsage)
   time.sleep(0.001)
  code=process.wait()
 elapsed=time.perf_counter_ns()-start
 assert code==0 and 'X07-07 actual CPU scene' in stdout_path.read_text()
 assert samples and peak_working and peak_private
 row={'process_launch':iteration,'elapsed_ns':elapsed,'peak_working_set_bytes':peak_working,'sampled_peak_private_bytes':peak_private,'samples':samples,'exit_code':code,'archive_log':'dev_log/P09/logs/'+stdout_path.name}
 runs.append(row);print(json.dumps(row),flush=True)
result={'scope':'Five fresh process launches of the actual CPU projection test. Includes DLL load and process startup; not in-process warmed latency or allocation count. Windows peak working set; sampled private bytes may miss short transients.',
 'capacity_contract':{'hub_limit':2,'iterations_per_process':32,'same_key_two_users_share_one_projection':True,'rejected_third_key':True,'drained_to_zero_each_iteration':True,'evidence':'Unmodified C++ assertions in editor/tools/scene/projection/test/projection.cpp; capacity is an asserted contract, not a measured allocation count.'},'runs':runs}
args.output.write_text(json.dumps(result,indent=2)+'\n')
