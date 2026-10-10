# F3 completion verification — PASS

Date: 2026-10-11. This report closes the remaining F3 performance gate under the
user-authorized fixed-binary evidence-reuse procedure. It does not change the
historical PARTIAL report or authorize F4.

## Identities and scope

| Identity | Commit |
|---|---|
| Production implementation I, unchanged | `0fbbbd6db3aa4ad53283522e4a54d96556b5b478` |
| Original F3 verification, retained PARTIAL | `41b4f25d973239098aac801f147daf09ab3ac640` |
| New diagnostics / impact-contract commit | `01cb46035a83eb71d7bbb88c5e5f2d21fe43b341` |
| Comparison A: F2-FIX implementation | `946cf0866eda3566abc2fe2d06581ff868e157c3` |
| Frozen V1 | `a669409a289a6fa4092f21176397795b1cdb7f3e` |

The tools commit adds exactly four files:

```text
cmake/render-v2-bootstrap/diagnose_f3.py
cmake/render-v2-bootstrap/test_impact_f3.py
docs/render-v2/F3_PERFORMANCE_INVESTIGATION.md
docs/render-v2/F3_TEST_IMPACT.md
```

This verification commit adds only this report. No production source, benchmark
workload, CMake target, Shader input, public ABI or historical report changed.

Independent clone:
`D:/LuxQualification/render-v2-f3-performance-01cb46035/source`.
Created with `clone --no-hardlinks --no-checkout`, then detached at the tools commit.
Tracked-snapshot validation and clean-source checks passed before qualification.
The diagnostic was executed from that clone against the original qualified binaries;
neither production tree was rebuilt or patched.

## Preserved evidence and binary identity

Original archive:
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F3/0fbbbd6db3aa4ad53283522e4a54d96556b5b478/`.
All 2,764 manifest entries still match. Original manifest SHA-256:
`9e49fec0f812236494ccac401cfc97ace8db7b7d26295d90871aa7b1953e1992`.

The original 256-pass p50 **+11.4478593%** and p95 **+12.2421362%**, all paired
samples, `performance-investigation.json`, and unfavorable development attempts
remain intact. They are not reclassified as historical PASS.

Scale benchmark SHA-256, checked against original qualification and after this run:

```text
A f2a424ab0255f41adbd3382c39bad2664b3d9303bf64d22a852799b7d575b938
B c6c1623274f1dfd13dc56dd8058eedd8ae7f7b6a5ce4dc36da6a813623387361
```

Original binding-benchmark hashes also match. Actual A/B objects, commands and
disassemblies were preserved. `Plan.cpp` and ScaleBenchmark sources are unchanged
between A/B, with equivalent compiler flags. Plan object machine code is not
identical: four of 2,872 code sections differ, including format-classification
inlining/calls and captured-field stride. Code bytes are 183,699 / 183,623.

Measured layouts and explanation are in `F3_PERFORMANCE_INVESTIGATION.md` and
`inspection/inspection.json`: resource/pass/use sizes remain 152/264/136 bytes;
CapturedFieldBinding grows 328→336 and typed clear 16→20. This fixture has no
captured fields. Its 256-pass allocation count/bytes remain **31,760 / 9,070,980**
for five measured compiles on both binaries, with equal workload and result counts.
No vector-growth or copied-binding increase was found for this workload.

## Independent paired performance result

Predeclared final protocol: 80 process pairs each for AA, BB and AB at 256 passes,
then 12 AB pairs each at 128 and 512. No sample was excluded or rerun for a preferred
result. The earlier 40- and 60-pair batches are preserved and included in the decision.
Total: 1,128 processes, of which 528 belong to the final independent qualification.

Each child used affinity mask 4 and ABOVE_NORMAL priority, verified through Windows
APIs. AB/BA order alternated by pair. Original workload, warmup and repeat counts
were unchanged. No build or GPU test ran concurrently. Confidence intervals use
fixed-seed bootstrap resampling of whole process pairs; inner samples are not
treated as independent processes.

Final 256-pass results (percent differences):

| Pair | Paired median p50 [95% CI] | Paired median p95 [95% CI] | Pooled p50 / p95 |
|---|---|---|---|
| AA | −0.022 [−0.428, +0.338] | −0.616 [−1.496, +0.187] | +0.113 / −1.674 |
| BB | +0.018 [−0.400, +0.411] | −0.491 [−1.444, +0.468] | −0.071 / −2.377 |
| AB | −0.543 [−0.972, −0.288] | −0.084 [−1.067, +0.341] | **−0.488 / +0.591** |

AB pooled values: p50 **2,827.7→2,813.9 µs**; p95 **3,080.3→3,098.5 µs**.
Pair-cluster 95% intervals for pooled changes:
p50 **[−0.707%, −0.219%]**, p95 **[−3.002%, +3.675%]**.
Both upper bounds are below the 5% gate without a waiver.

The final AB per-pair p50 range is [−6.386%, +7.492%], and p95 range is
[−29.681%, +54.567%]. These outliers remain in the data. The initial batch's
+168.43% p95 outlier also remains. AA/BB noise is not subtracted from AB.

Earlier AB paired medians:

| Batch | Pairs | p50 | p95 |
|---|---:|---:|---:|
| Initial | 40 | −0.388% | −0.362% |
| Replication | 60 | −0.083% | −0.367% |
| Independent final | 80 | −0.543% | −0.084% |

Limited neighbor controls: 128 pooled p50/p95 **+0.172% / +4.371%**;
512 **−0.325% / +2.889%**. These small controls have broad p95 intervals
([−0.959%, +19.018%] and [−3.383%, +51.159%]); they are not a new high-precision
qualification of every graph size. Existing qualified 128/512 results remain valid.
No stable 256 regression remained to justify changing workload for 224/240/272/288.

## Attribution and its limits

Decision **B**: the previous >5% regression cannot be reproduced as a persistent
production difference across the fixed-binary AA/BB/AB batches. This is supported
by unchanged binaries, pair-level distributions and final confidence bounds, not
by a single successful repeat. No production optimization or algorithm rewrite
was justified or performed.

The exact transient cause of the historical batch is **not established**. This
report does not claim to have proven a specific scheduler, cache, ASLR or antivirus
cause. Nor does it claim the differing machine code has zero cost under every
environment. It establishes the current comparative performance gate at the
specified workload and host, retaining the historical negative observation.

Host: i7-13700KF, 16 cores / 24 logical processors, High Performance power plan.
Final pre/post processor-performance samples were approximately 152.7% / 149.3%
of nominal frequency. They are snapshots, not continuous frequency traces.
Median whole-process cycles were 77,232,788.5 / 77,014,040; these include startup
and teardown and cannot isolate compiler instructions. Process CPU time is coarse.
xperf exposed PMU sources but refused trace start with access denied (0x5).
Instructions/cache/branch-miss and context-switch attribution are **NOT_RUN**.
No privilege escalation or system power-policy change was made.

## Test impact and reused qualification

`qualification/impact/report.json` reports **REUSE_ELIGIBLE**. It records all four
paths' exact old/new Git objects, 300 dependency records per configuration, actual
Ninja inputs/deps/commands, CMake File API, compile/link and codegen closure, and
CTest commands. No changed path occurs in a target/test dependency. Unknown paths
or hits would require integration review. Production/frozen Git trees match.

All 266 binary/object/SPIR-V artifacts in each existing configuration match the
investigation lock before and after qualification. Original benchmark hashes also
match historical receipts. The old archive did not hash every test executable;
the new artifact lock does not retroactively claim that it did.

Reused, not newly executed:

- Normal CTest **137/137 PASS**: original `test.json`, `test.log`, `closure/tests.json`.
- Full applicable ASan **137/137 PASS**: original `asan-test.json`, `asan-test.log`.
- Native GPU **validation 0 error**, W01 Tonemap/Blur/Composite and other F3
  readbacks: original GPU logs, generated SPIR-V/reflection and readback manifests.
- Steady Binding **zero C++ allocation** and passed p50/p95 comparison:
  original `performance/performance.json`, with both binary hashes unchanged.
- Existing Graph version/Hazard/Scope/oracles, header/Concept and dependency gates.

No test was removed or weakened. Full build/CTest/ASan/GPU reruns are **NOT_RUN
anew**, because no production input changed. This follows the explicit impact-based
policy; it is not a functional gate exemption. Diagnostic scripts were syntax
checked and actually executed from the independent clone. A new production I or
integration rebuild is unnecessary for this tools-only change.

Core, Transport, Vulkan, Graph and all production tree identities are unchanged.
The frozen Legacy tree retains the previously proven 719-file identity; no repeated
V1 build was needed. FINAL and all historical reports are unchanged. The original
user worktree's six file hashes, HEAD, branch, status, staged/unstaged diffs and
untracked list remain byte-identical to prior protection receipts.

## New evidence anchor and final status

Evidence root:
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F3/performance-completion/`.
Manifest: **3,469 files**, SHA-256:
`c20571cb28083a4e909699285c3b55d3278c1fe5440e31c7d8844db202fea1e1`.

Key entries: `inspection/`, `initial-256/`, `replicate-256/`,
`qualification/final-256/`, `qualification/control-{128,512}/`,
`qualification/impact/report.json`, `qualification/conclusion.json`,
`baseline-lock.json`, environment receipts and `pmc-attempt.log`.
Every pair has command, exit code, raw stdout/stderr and timing data. The manifest
also includes the inspection/qualification/finalization scripts and full tools diff.

```text
F3 = PASS
Historical F3 verification 41b4f25d = PARTIAL (unchanged)
Production implementation I = 0fbbbd6db3aa4ad53283522e4a54d96556b5b478 (unchanged)
Native RenderGraph = NOT_IMPLEMENTED
Runtime / FrameLoop = NOT_IMPLEMENTED
Full Material integration / I42 remainder = deferred to F8
V2_PRODUCT = EXPECTED_UNAVAILABLE
F4 = NOT_AUTHORIZED
STOP — await user review
```
