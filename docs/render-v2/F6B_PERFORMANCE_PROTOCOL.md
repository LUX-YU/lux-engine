# F6.b bounded performance protocol — frozen before measurements

A = F6.a I bf3223038d415ed6912dcea04dc8f4d195becd3e, independently built or hash-verified locked binaries.
B = final F6.b implementation I, recorded before execution. C++20 MSVC RelWithDebInfo, same dependencies,
Shader bytes, GPU/device options, descriptor counts, warm-up and timing boundaries.

For each original ShaderBenchmark and NativeGraphBenchmark workload, run exactly 8 AB pairs,
3 AA pairs and 3 BB pairs: 28 independent processes per workload, 56 total. Seed 20261012
fixes pair/configuration ordering and AB/BA orientation before seeing samples. No replacement runs,
no sample removal, no added pairs. Exit failures remain failures with raw logs.
Affinity logical CPU 2 (mask 4), AboveNormal priority; record actual power plan, CPU/GPU/driver,
compiler/binary/Shader hashes and process times. No concurrent build, GPU test or profiler.

Preserve every per-operation sample, process p50/p95/max, allocation counts/bytes and paired ratios.
For each endpoint report median paired percentage and percentile-bootstrap CI95 over the 8 AB
process pairs (10000 resamples, fixed seed 20261012); AA/BB describe baseline noise separately.
The four hot endpoints are fixed_native_binding_recipe and native_graph_record_submit_256x256
p50 and p95. Require CI95 upper bound <= +5%, valid oracles and zero warm first-party allocation.
An interval overlapping the threshold is PARTIAL, not a retry trigger. A stable attributable
regression requires a separate minimal source analysis. Historical F4/F5 debts remain deferred.

Also retain original layout/relocation/candidate cold metrics. Multi-owner cold assembly/binding,
actual native object counts and 2/3 FIF warmed execution establish new baselines without fabricated
old-equivalent percentage claims. Vulkan descriptor allocation bytes unavailable from the native
API are labelled unavailable, distinct from measured first-party heap and VMA payload/reservation.

Full GPU correctness uses Validation/SyncValidation separately. No general VTune rerun; only a
bounded targeted investigation if a repeatable new hotspot is observed. Timing under profiling
cannot replace these formal samples. Protocol hash, executable identities and randomized schedule
are saved outside the source tree before starting the formal experiment.

AA/BB controls are also flagged unresolved if their paired median exceeds +/-5% or their CI does not
include zero. This conservative diagnostic can keep the result PARTIAL; it never triggers extra pairs.
A child failure stops this one experiment with retained logs and an incomplete process count, not a
replacement child or a fabricated 56-process success. Bootstrap CI is process-paired, not frame-pooled.
