# F5-C performance protocol — preregistered

This protocol is fixed before observing any new formal A/B result. It does not
replace F5's PARTIAL result or waive the deferred F4 cold debt.

## Prior evidence and measurement limits

Source: F5 I `e662ff59844cf69f00d94341768df58ddc7f44b0`, archived
`performance/rows.jsonl`, SHA-256
`23cb06a7aaa5660cc28997b89bd315c7cb594badd80acdb6f73dba942a789c3a`.
The complete analysis is retained externally as `prior-performance-analysis.json`.
QPC frequency was 10 MHz. Record/submit samples are quantized to 100 ns, about
0.4% of the 23–27 us process medians; this alone does not explain a 5% tail gate.
Each process has only 100 record samples (five observations above the p95 rank).
A/A p95 values ranged 33.9–55.3 us; B/B 32.4–49.6 us. Their paired p95 CIs were
[-7.6446%, +26.0465%] and [-4.9896%, +9.8214%]. These demonstrate instability,
not its cause. Old evidence lacks per-sample frequency, scheduler and cache
counters; Windows/driver/frequency causation remains NOT_PROVEN.

## One bounded formal experiment

- A: locked F4-C-P `92b126f67873ccbb576d0ffc5f1330964d6f84f6` binaries.
  B: independent build of the final F5-C I. Hash both executables and inputs.
- Keep original shader and native graph benchmark workloads, sample counts,
  warm-up, timer boundaries, shaders, quality and native operations unchanged.
  No timing amplification or new B workload is compared against old A.
- Per executable: exactly 21 AA pairs, 21 BB pairs, 41 AB pairs, one process per
  arm, alternating AB/BA. Two executables give 332 processes. Increase independent
  pairs rather than changing the locked binaries' inner work. Seed: 20261016.
- Randomize workload/control cells within each block with that seed. Affinity
  mask 4, AboveNormal process priority, existing High Performance power plan.
  No concurrent builds, GPU tests or GPU diagnostics. Do not change the driver
  cache or select a favorable time window. Record start/end, process CPU time,
  I/O, working set when available, QPC and power plan; missing OS data is explicit.
- Keep all raw samples, abnormal tails, commands, statuses and pairs. No exclusions,
  pooled-only conclusion, optional stopping or repeat-until-PASS. A child failure
  stops the experiment with FAIL/PARTIAL and retained evidence; no silent retry.
- Each pair's p50/p95/max relative difference is reported. Use paired median
  percentages and percentile bootstrap CI95, 20,000 resamples, seed 20261016.
  Preserve process distributions separately from within-process distributions.
- For both hot cases, p50 and p95 AB point estimate **and CI upper bound** must
  be <= +5%. AA/BB median magnitude <=5% and their CI must include zero.
  Unresolved controls or an interval crossing the gate means PARTIAL, not PASS.
  Actual attributable >5% regression requires a correction; no waiver here.
- Both hot workloads require zero first-party allocation count/bytes. Cold groups
  emitted by the unchanged executables are retained as diagnostics only. They
  cannot close or reinterpret F4's deferred cold debt.

## Resource and Runtime experiments

Cold footprint: maximum capacity 8 Views, actual 1–4, FIF 2/3, 1080p and supported
4K; same extent and a second half-size extent. Report actual slots, native object
counts, VMA allocation bytes/count and block bytes separately. Keep the old
allocation-policy executable/source hashes. Count complete construction, including
import images, scratch, descriptor allocation and failure rollback. Additional
small GPU fixtures check growth, reuse, resize/path transactions and completion.

Import scale: fixed 1/8/32/64 imported buffers, 1/4 consumers and FIF 2/3. Use the
actual production preparation algorithm; report its CPU p50/p95 and allocation
counts. This diagnostic includes binding preparation, not pure validation only.
Do not alter production validation without a demonstrated significant bottleneck.

Runtime baseline: unchanged F5 real shared-buffer workload, 1 Scene/1 View,
1 Scene/4 Views, 2 Scenes/4 Views; three processes each, 24 warm-up and 240 measured
frames. Keep CPU frame/preparation, queue timestamps, pacing, cache and memory
metrics and independent pixel oracle. It is a mechanism baseline, not product FPS.

Normal and Full ASan final qualification use the exact I; profiling targets remain
distinct from production. Evidence lives under `D:/LuxQualification/render-v2-f5c/`
(E: archive volume lacks space). Archive identity, not a mutable directory name,
determines qualification. Only the independent report may declare the result.
