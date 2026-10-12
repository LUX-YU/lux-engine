# F6 VTune diagnostic protocol

Frozen before collection. This is attribution, not a replacement latency gate.

Inputs: F4-C-P `92b126f67873ccbb576d0ffc5f1330964d6f84f6` Native;
F5-C `e0c31de6678394c37ad7ed1ffc2d77231bccb9cc` Native and Runtime 1 Scene/4 Views.
Hash executables, PDBs, shader inputs, production commits and any separate harness.
Archive CLI help/version, Windows/CPU topology/affinity/power plan, GPU/driver,
compiler configuration and symbol/source lookup results outside the source tree.

Collection budget (no retry-until-favorable):

| Mode | Maximum |
|---|---|
| Original executable software feasibility | 3 runs, 30 seconds each |
| Warm software Hotspots | 3 runs, 180 seconds each |
| B fixed Binding, only if unresolved in primary captures | 1 run, 180 seconds |
| Hardware Hotspots, only if actually available | 2 runs, 120 seconds each |
| Threading, only after evidence of relevant waiting | 1 run, 180 seconds |
| Uarch, only with legal PMU and a CPU-bound hotspot | 1 run, 120 seconds |

VTune 2026.3.0 build 632627 reports a fixed 10 ms software interval. Request stacks
using the installed CLI's supported knobs. Driver service status alone is not
PMU evidence. Do not install drivers, disable security or substitute zero for an
unsupported event. Preserve command, return code and original diagnostics.

No builds, other GPU tests or performance workloads may overlap collection.
Record fixed affinity and process priority. No tuning after inspecting results.

If short fixtures under-sample the warm path, a default-OFF DEVELOPMENT_ONLY
entry may extend execution, with production source unchanged and its own hash.
Initialize once, warm up, preserve per-operation GPU work, pacing, completion,
backpressure and output oracle. Storage must remain bounded. The original 264
frame profiling wrapper and original formal benchmarks must remain unchanged.
Do not grow its query pool without bound or mix timestamp-wrapper overhead with
unwrapped production results. Separate initialization, warm execution and oracle.

Report self/inclusive CPU samples, call stacks, source/module attribution and
coverage. Sparse functions cannot be ranked precisely or described as free.
Separate CPU busy, pacing, queue completion and other waits using actual evidence.
Driver functions without symbols remain module-level/UNKNOWN. Hardware metrics
must identify core type, events and multiplexing. No Intel GPU results stand in
for NVIDIA/Vulkan GPU timing.

All failures and samples are retained. Insufficient coverage is INCONCLUSIVE;
unavailable collection is VTUNE_NOT_AVAILABLE, neither blocks F6.a. Production
fixes require a separate proposal. Historical p50/p95 and debt states are untouched.
