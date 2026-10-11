# F5 bounded performance protocol — before qualification measurements

Entry remains 719f3d06e077e639901ae598f230653c5312c905. F4 cold uncertainty
is retained, not converted to a performance pass. Development GPU allocation
checks are correctness diagnostics; final timing uses the locked independent I.

## Comparable native regression

- A: locked F4-C-P I 92b126f67873ccbb576d0ffc5f1330964d6f84f6 normal binaries.
- B: final F5 I normal binaries, identical MSVC configuration/flags and driver.
- Preserve executable, shader, source workload and command hashes. Do not compare
  ASan/validation timing with normal performance.
- Run unchanged full `shader_benchmark` and `graph_benchmark` workloads. Test
  assembly migration must include both native code and invocation construction;
  excluding new backing construction from cold totals is prohibited.
- Fixed 11 A/A pairs, 11 B/B pairs and 21 A/B pairs **per workload**, one process
  per arm, alternating AB/BA order; fixed schedule seed 20261015. CPU affinity
  mask 4, AboveNormal priority, no concurrent build/GPU qualification.
- Retain every raw per-process sample, command, exit code and pair. Report paired
  p50/p95/max percentages and 20,000-resample bootstrap median CI95 (fixed seed).
  No outlier deletion, pooled-only comparison or running until a favorable result.
- Primary hot gates: fixed_native_binding_recipe and
  native_graph_record_submit_256x256; warmed allocation count/bytes must be zero.
  p50/p95 >5% unexplained regression stops qualification. If uncertainty cannot
  distinguish the gate, report PARTIAL with all controls, not PASS from one run.
- Cold groups are recorded in this same bounded run because native construction
  changed. They inform the carried F4 debt; this run cannot unilaterally waive or
  erase that debt. No additional unbounded cold experiment is authorized.

## New Runtime baseline

There is no equivalent old N-View Runtime binary. Establish a baseline, do not
claim a percentage improvement over an incompatible single-invocation fixture.
Use the same real multi-queue GPU workload and independent numeric oracles as
the functional fixtures. Fixed 1/4 View and 2 Scene configurations, warm-up 24
frames then 240 frames each, three processes each; preserve all samples.

Record CPU frame record/submit, import validation, pacing intervals/deadline
misses, cache hits/compiles, real GPU timestamps and VMA backing peak bytes.
Any diagnostic instrumentation must compile only into a clearly identified
profiling target, use the real Runtime/Native algorithms, and never appear in
the normal production library or public ABI. Record its overhead/method rather
than presenting CPU elapsed waits as GPU time. Missing metrics are NOT_RUN and
must remain visible in the final qualification decision.

Warm production correctness fixtures additionally verify zero first-party C++
allocations, no new native shader/layout/pipeline creation and no repeated graph
compilation. Cold resize/path replacement is separately classified.
