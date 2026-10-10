# F3 targeted performance investigation

This work continues F3; it does not create another architecture stage or authorize F4.
Entry verification: `41b4f25d973239098aac801f147daf09ab3ac640`.
Production implementation remains `0fbbbd6db3aa4ad53283522e4a54d96556b5b478`.
Comparison implementation A: `946cf0866eda3566abc2fe2d06581ff868e157c3` (F2-FIX).

## Evidence preservation

The previous F3 report remains PARTIAL. Its 256-pass pooled p50 +11.4478593% and
p95 +12.2421362%, all paired samples and `performance-investigation.json` remain
unchanged. The 2,764-file archive was rehashed successfully; its manifest SHA-256 is
`9e49fec0f812236494ccac401cfc97ace8db7b7d26295d90871aa7b1953e1992`.

New evidence root:
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F3/performance-completion/`.
`inspection/` retains both actual objects, disassemblies, compiler commands, benchmark
executables and layout probes. No original binary or measurement was replaced.

## Attribution before changes

Neither production code nor the formal ScaleBenchmark workload was modified.
The original five measured compiles per 256-pass process and its warmup remain intact.
Both binaries run as separate processes with mask 4 (logical CPU 2) and ABOVE_NORMAL
priority. The revised diagnostic checks these settings on each child, records wall
time and whole-process CPU cycles/times, retains every sample and alternates pair order.
Whole-process counters include startup/destruction; they are not isolated compiler PMU counts.

Measured MSVC x64 layouts (size/alignment):

| Type | F2-FIX | F3 |
|---|---:|---:|
| GraphResource | 152/8 | 152/8 |
| GraphPass | 264/8 | 264/8 |
| GraphResourceUse | 136/8 | 136/8 |
| CapturedFieldBinding | 328/8 | 336/8 |
| Clear array / ColorClearValue | 16/4 | 20/4 |

The formal fixture has empty captured-field vectors. Its allocation count/bytes and
resource/version/hazard workload are equal across binaries. There is no evidence for
extra vector growth or copied captured-field storage in this workload.

Plan.cpp source and compiler options are unchanged, but its machine code is **not**
identical. Four code sections differ: analyze, logicalIdentity, normalize, and
textureAspectMask. The format-classification header affects inlining/calls; field
stride changes affect code generated for captured bindings. Raw COFF code comparison
and normalized disassembly are archived. This does not prove that these differences
caused the earlier 256-only slowdown, and no algorithm-complexity claim is made.

The host is an i7-13700KF, 16 cores / 24 logical processors, High Performance power
plan. A sampled processor-performance counter was about 155% of nominal frequency.
That snapshot cannot reconstruct frequency or scheduling during the earlier run.
Available xperf PMU sources include instructions, cycles, cache and branch misses;
attempting the scoped trace failed with access denied (0x5). No PMU result is claimed.

## Exploratory replications (not the final qualification)

The first batch used 40 pairs each of AA, BB, AB. AB paired median changes were
p50 -0.388%, p95 -0.362%. The second, independently launched batch used 60 pairs each;
AB changes were p50 -0.083% (95% paired bootstrap CI [-0.666%, +0.206%]) and p95
-0.367% (CI [-2.143%, +2.838%]). Its pooled changes were -0.432% / +1.531%.
All outliers, including a first-batch AB per-process p95 outlier of +168.43%, remain.
AA/BB are noise controls, not values subtracted from AB to manufacture a pass.

These batches do not reproduce a persistent 11–12% production regression. They do
not invalidate the historical measurement or identify its exact transient cause.
They justify testing the user-authorized measurement/environment conclusion, rather
than rewriting Plan.cpp without a reproducible production defect.

## Frozen independent protocol

After the tools/docs commit, use `clone --no-hardlinks --no-checkout` and detach at
that commit. Run the committed diagnostic on the original, hash-verified A/B binaries:

- 256 passes: 80 pairs **each** of AA, BB and AB; fixed AB/BA alternation.
- 128 and 512 passes: 12 AB pairs each as neighboring controls.
- Preserve stdout/stderr, commands, exit codes, scheduling settings, raw samples,
  every pair's change, distribution and deterministic bootstrap confidence intervals.
- Report both the median of paired per-process changes and pooled percentiles.
  Resample whole process pairs for confidence intervals, not correlated inner samples.
- Do not rerun until a preferred result appears, remove outliers, or change workload.
- Expand to neighboring 224/240/272/288 only if the 256 regression stably recurs.
- No builds or GPU tests run concurrently with measurements.

The completion report must evaluate all batches, including adverse observations.
If unexplained >5% remains, qualification stays PARTIAL. No correctness-cost waiver
has been requested or granted. Final results belong in a separate verification commit.

Native RenderGraph = NOT_IMPLEMENTED. F4 = NOT_AUTHORIZED. Product = EXPECTED_UNAVAILABLE.
