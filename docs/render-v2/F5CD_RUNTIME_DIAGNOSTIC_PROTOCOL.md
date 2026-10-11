# F5-C-D Runtime diagnostic — frozen before measurement

This is attribution, not a replacement performance gate. F5-C remains PARTIAL:
Native Record/Submit p95 CI95 upper bound +5.5749% > +5%. F4 cold debt remains
PARTIAL; F6/F7 are NOT_AUTHORIZED. Production sources and historical reports are
read-only. Only this protocol, the diagnostic script and a later verification
report are initial changes.

## Sources and construction

- Parent: `c14baf48500ba94d2beaead7d86d853a868726fc`.
- Runtime A: `e662ff59844cf69f00d94341768df58ddc7f44b0`.
- Runtime B: `e0c31de6678394c37ad7ed1ffc2d77231bccb9cc`.
- This is **not** the F4-C-P versus F5-C formal Native benchmark comparison.
- Independent no-hardlink clean clones of A/B, same current MSVC/C++20,
  RelWithDebInfo, Ninja, dependencies, Vulkan SDK and configure arguments.
  Build the existing benchmark and its actual dependencies, `-j 4 -- -k 0`,
  then verify no work. No concurrent builds or GPU execution.
- RuntimeBenchmark.cpp, ProfileFrame.cpp and ProfileNativeFrame.cpp must retain
  equal A/B Git blobs. Compare actual compiler flags, link inputs, generator jobs
  and generated Shader/SPIR-V hashes, not just source filenames. Different
  production objects are expected and indexed; unexplained tool/workload
  differences mean NOT_COMPARABLE.

The original profile does not print actual slots. The script mechanically derives
one test-only ProfileFrame TU from each exact source: it resolves the original
production include and adds cold metadata in the existing `if (!queries)` block,
before any original timed frame. It prints actual target-slot storage, native
diagnostics, cell counts and capacities. No per-frame branch, timer, production
target, public ABI or native algorithm changes. The CMake project-include hook
replaces only the existing profiling target's source. Derived input, transformation,
compiler command and output hashes are retained. This metadata variant is disclosed;
historical executable hashes are not claimed identical to this build.

## Fixed experiment

Exactly three configurations: 1 Scene/1 View, 1 Scene/4 Views, 2 Scenes/4 Views.
Keep original 24 warm-up + 240 measured frames, FIF=2, 120Hz requested pacing,
Shader, compute/transfer, query submissions, readback, independent pixel oracle,
validation-off benchmark setting and timer boundaries unchanged.

For each configuration: 4 AB pairs, 2 AA pairs, 2 BB pairs = 16 processes.
Total **48 processes**, one attempt each. Seed **20261011**; precompute four
blocks, shuffle configuration/control cells within each block, alternate A/B and
B/A arm order by block. Freeze the complete schedule before the first child.
No observed-result increase, exclusions, replacement runs or favorable-window
selection. A child failure preserves stdout/stderr/exit and ends the experiment.

Use affinity mask 4, AboveNormal priority and the existing High Performance power
plan; no concurrent build, GPU test, capture or load test. Record CPU/GPU/driver,
compiler and inputs, QPC frequency, UTC start/end, per-child CPU time, I/O and
working set when available (otherwise explicit unavailable/error). Do not clear
driver caches or attribute noise to them without evidence.

Before run, freeze protocol/script/generated metadata TU, executable and Shader
input SHA-256. The runner refuses an existing experiment directory. It records
all 240 samples, p50/p95/max, Native slot metadata, actual compilation/cache/reuse
and wait/deadline counts, warm allocations, exit status and pixel-oracle success.
The queue timestamps include submission gaps, not pure Shader execution time.

## Analysis and stopping

Keep per-process A/B pairs for every metric; print every difference and AA/BB
control dispersion. Report process medians/ranges separately from paired changes.
Do not pool frames to manufacture a stronger significance claim; no +5% PASS
classification and no small-sample CI used as qualification.

ProfileFrame encloses executeRuntimeFrame, and its import accumulator sums the
nested prepareSlot wrappers within that invocation. Compute remaining CPU time
**per matching frame sample**, then its quantiles; do not subtract two p95 values.
Count/query static call paths separately from timed measurements. Classify the
historical large p50 change as NOT_REPRODUCED only if this experiment fails to
reproduce that magnitude/direction; smaller unresolved differences are INCONCLUSIVE,
not proof of equivalence. A reproducible attributable regression requires source,
call counts and a proposal, not production edits.

After all 48 results, at most one bounded local instrumentation experiment may be
specified if existing timers cannot distinguish a material difference or the
pending-cell cases. Explain its need first; apply equal observation to A/B and
report separately. Never mix it into the 48-process results or old formal gate.
No repeat of the 332-process Native experiment. No production correction in this
authorization; a proven correctness/ownership conflict stops the affected work.

## Qualification and inherited evidence

Commit I freezes this protocol/script before measurement. Independently clone I,
run/check the diagnostic using exact A/B builds, preserve all inputs and output
outside source under `D:/LuxQualification/render-v2-f5cd/`. V only adds
F5CD_VERIFICATION.md. Verify allowed diff, I parent, 719 Legacy blobs/modes/sizes,
all production/architecture/history objects, original seven dirty files and both
user checkout states. Manifest all evidence with SHA-256.

Normal/Full ASan 175/175 and validation/SyncValidation are **inherited** from
F5-C, not rerun by these benchmark processes. This task does not require full
CTest/ASan/GPU qualification without a production change. Platform/SDK/product
tests not executed are NOT_RUN. Report Q1–Q5, recommend ELIGIBLE_WITH_EXPLICIT_USER_DEFERRAL
or HOLD from evidence, push I/V and STOP. Neither recommendation authorizes F6.a.
