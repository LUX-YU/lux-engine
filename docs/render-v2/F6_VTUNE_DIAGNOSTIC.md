# F6 — independent VTune diagnostic

Diagnostic conclusion: **INCONCLUSIVE** for a production Import/Binding/Runtime CPU
hotspot or regression. The bounded collection completed; it does not close either
historical performance debt and does not qualify F6.b. No Runtime/Native production
file was changed for this diagnostic.

## Identity and evidence

Protocol: [F6_VTUNE_PROTOCOL.md](F6_VTUNE_PROTOCOL.md). The pre-collection byte hash was
`9560ca97fb5b7c6554578fbc890ffa0ec0af5afe4e4c4e550492241b97f76c48`.
The long-entry script was frozen before warm collection with byte hash
`90b9cc62caae6040b1e9177cb59073a755a7e37841c08f8ec39fae93796135a0`.
These hashes include the construction checkout's line endings; Git blob identities
and the clean-clone versions are also retained. This is not a claim of byte equality
between CRLF and LF checkouts.

Raw root: `D:/LuxQualification/render-v2-f6a/`. Exact commands/exit codes are in
`vtune-*.json`; stdout/stderr in matching `.log`; untouched collection databases under
`vtune/`; EXE/PDB/source/harness identities in `vtune/warm-pre-collection.json`.
The file-level manifest and its hash are linked by the accompanying F6A receipt.

| Workload | Exact historical production source | Warm EXE SHA-256 |
|---|---|---|
| A Native | `92b126f67873ccbb576d0ffc5f1330964d6f84f6` | `723e8adeec379fa66e06756ac51c1289ac3ed7835c7279fff957a734fba96800` |
| B Native | `e0c31de6678394c37ad7ed1ffc2d77231bccb9cc` | `0935d5c97d138127e8a42b652d7d0cc86cb31b66226807fa3409e1ef108dc5d9` |
| B Runtime 1 Scene / 4 Views | same B | `47f096ebc0db9b2bb5fec8cce6de1ff9bdc1bf12be58a25745dd425e74acea9b` |

These are new diagnostic harness binaries over exact clean historical sources, not
the old locked benchmark binaries. The original short entries were sampled separately.
RelWithDebInfo MSVC 19.44.35228, generated shader inputs, compile commands, link commands
and PDBs are archived. Source/consumer mappings resolve engine frames; loader/driver
private symbols do not resolve completely.

## Tool and collection boundary

VTune `2026.3.0 build 632627`, Windows 11 build 26200, i7-13700KF, 16 cores / 24 logical
processors, NVIDIA RTX 4070 Ti / 591.86. Software Hotspots uses the CLI's fixed 10 ms
mode with stack collection. Affinity mask 4 selects logical processor 2 (CPU-set
efficiency class 1); AboveNormal priority is inherited by the target. CPU-set/QPC
facts are in `vtune/system-topology.json`.

High Performance power plan was observed **after** collection, not timestamped before
each run. An OBS graphics-hook module was loaded; no OBS process was present at the
post-collection inventory. Those facts do not establish an interference-free machine.
No build or competing qualification GPU workload was launched during captures.

Executed once each: three short-entry feasibility captures, three bounded warm
Hotspots captures, one conditional Native Threading capture. All returned zero and
preserved their real GPU oracles. Native warm loops retain the original timestamp
submissions, readback and completion; Runtime uses production without profiling
wrappers, 24 warm-up frames and 90 bounded blocks of 240 frames. No growing query pool.

One hardware Hotspots attempt returned exit 1: VTune could not enable hardware
event-based sampling/tracing and requested a loaded sampling driver / administrator
privileges. AdministratorPrivileges=false is recorded. **PMU = NOT_AVAILABLE**;
instructions/cycles/cache misses/uarch = NOT_RUN. No driver, privilege or system
security setting was changed. No second hardware capture or repeated-to-PASS run.
Optional fixed-binding sampling was not exercised: this run does not provide a
standalone binding cost estimate.

## Observations, not latency qualification

| Warm capture | Elapsed s | VTune CPU time s | Effective s | Classified spin s | Paused s |
|---|---:|---:|---:|---:|---:|
| A Native | 182.750819 | 63.410819 | 20.295316 | 43.115502 | 2.957876 |
| B Native | 182.432974 | 59.702027 | 19.418675 | 40.283353 | 14.082509 |
| B Runtime | 182.920505 | 3.246332 | 0.626527 | 2.619804 | 2.970882 |

Do not divide or compare these as an A/B speedup: operation counts, paused preparation
and sampling overhead differ. These are attributed CPU times, not Invocation p95s or
NVIDIA GPU kernel measurements. GPU timing remains the existing Vulkan timestamp path.

Native bottom-up reports attribute 55.360758 s (A) and 53.075148 s (B) to an unresolved
`vulkan-1.dll` function. Call stacks separate its callers: `SubmissionQueue::wait`
accounts for 41.607643 / 39.385652 s; Native queue submit for 7.244368 / 6.723279 s;
harness timestamp queue submit for 6.326292 / 6.803561 s. This locates substantial cost
under loader/queue completion and submission, but private symbols cannot distinguish
loader, driver internals, interception or the precise waiting mechanism.

Source anchors in B: `vulkan/src/transfer/Submission.cpp:187,331`,
`vulkan/src/graph/Record.cpp:107,526,844,878`. The test's extra timestamp submissions
and synchronous readback contribute; their time must not be called pure production
record cost. No evidence here justifies removing a completion wait or Sync2 check.

Runtime top-down resolves `RenderRuntime::Impl::run` -> `WaitForMultipleObjects`
(`runtime/src/Runtime.cpp:485`), 2.205219 s self / 67.9% of recorded CPU attribution.
The harness `sleep_for` contributes 0.335246 s; unknown stacks 0.555213 s. The engine
`executeRuntimeFrame` -> Native submit subtree is only 0.926239% of the reported
inclusive CPU total. It is too sparse to rank `validateImports`, descriptor preparation
or `has_pending_cells`. Source anchors: `runtime/src/Frame.cpp:45`,
`vulkan/src/graph/Frame.cpp:56,66,196,305`.

The warm Runtime completed 21,625 frames / 86,500 invocations, 64,875 Scene reuses,
one compile, three cache hits, 21,629 waits, five deadline misses, and **0 allocations /
0 bytes**. Four-view pixel checks passed. This is diagnostic functional evidence,
not a new pacing or 5% latency qualification.

Threading (182.924186 s) reports 0.7% effective utilization, 14 threads and
1,147.952 s aggregate wait. Waits sum across threads and exceed wall time legitimately.
AutoResetEvent accounts for 898.844599 s / 43,149 waits. Classified spin/overhead is
59.153 s, mostly loader attribution. VTune labels alone do not prove busy polling,
OS scheduling delay, a GPU kernel bottleneck or redundant synchronization.

Raw software sample counts are **NOT_AVAILABLE from the exported reports**: even
the attempted `-show-as samples` reports return CPU-time columns, not event counts.
All raw data and the attempted reports are retained. No count is inferred by dividing
time by 10 ms. Coverage is adequate to identify the large Native wait/submit subtree,
inadequate for fine Runtime preparation costs; unknown frames remain visible.

## Actionable limits and next responsibility

No deterministic correctness defect or demonstrated new production regression was
established by this diagnostic. This is not an all-platform safety claim.
The bounded run supports a narrow future investigation of loader/driver wait and
submit callers, with harness timestamp/readback cost explicitly separated. It does
not support a Runtime/Native rewrite or speculative micro-optimization. F6.b may
profile its actual multi-owner path after that consumer exists, under a separate
frozen protocol. No production fix is included here.

`VTUNE_DIAGNOSTIC = INCONCLUSIVE`; `F4_COLD_DEBT = PARTIAL — deferred`;
`F5C_RECORD_SUBMIT_P95 = PARTIAL — deferred`. This diagnostic does not grant F6.b/F7
authority and does not alter historical reports or any adverse sample.
