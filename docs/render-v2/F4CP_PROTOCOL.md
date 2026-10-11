# RD2-F4-C-P frozen measurement protocol

Authorized entry V: `6342d56caf826504278399e2430a2a4499d9d3c2`.
Existing production I: `5039fb42424dfda23bc7da518e7bbcf863f8c932`.
Scope: performance evidence and one UINT32 positive in the existing NativeGraphDraw fixture.
Production, architecture, previous reports and user edits remain read-only. F5/F6/F7 are not authorized.

## Prior evidence and measurement limits

The external preflight index hashes 540 existing performance/control files and analyzes all 13 comparison
reports, including the original unfavorable observations. No historical sample is overwritten or excluded.
The two unresolved observations remain C9 p95 +6.5574% and complete candidate p95 +5.9257%.

Actual QueryPerformanceFrequency is 10,000,000 Hz (100 ns). All archived raw samples of both cases have
100 ns granularity. At approximately 6 us, one tick is 1.67%; at 2.5 ms it is 0.004%. Thus timer precision
can materially quantize C9 but cannot explain millisecond candidate variation by itself.

Each Graph process measures 30 complete native compilations, with prebuilt Shader programs outside the
timed interval. C9 is a nested production timing of identity/candidate preparation. Each Shader process
measures 100 complete candidates after five warmups. Its helper reads four source files and two SPIR-V
files, constructs/validates the compiled variant, builds/compiles the logical fixture, exercises the
existing rejected format candidate and creates the accepted native program. These operations stay intact.

Cold means new candidate/native objects; it does NOT mean a flushed OS file cache or empty driver cache.
No explicit VkPipelineCache is supplied by the existing pipeline factory. Implicit driver caches remain
uncontrolled. No cache is deleted. Process startup and prior workload ordering are preserved. File-read,
reflection and driver costs cannot be attributed separately from the existing aggregate timer; no claim
of such attribution is permitted. No new benchmark or smaller workload replaces either complete workload.

Prior nine-pair AA/BB confidence intervals are wider than 5%. This bounded experiment increases independent
process replication to 31 pairs per cell (rather than merely replacing the previous nine samples). It
cannot remove 100 ns quantization or guarantee that process p95 from 30 observations becomes precise.

## Preregistered experiment (before any new A/B)

- Two workloads: unchanged NativeGraphBenchmark and ShaderBenchmark executables/arguments/work ordering.
- A(Graph): original F4 I `bd5399063907087e147ee159f24e1da6a87ab30f` locked executable.
- A(Shader): original F3 I `0fbbbd6db3aa4ad53283522e4a54d96556b5b478`, same original objects finalized
  with `/INCREMENTAL:NO`, archived in F4-C/relink-control/A. Retain original-link historical comparison too.
- B: final independent normal build of this test-only I. Record executables, static libraries, compiler
  commands and link inputs. Production Git objects must equal F4-C I; executable identity need not equal
  a different-path independent build. AA and BB use exactly the same binary on both sides.
- 31 pairs EACH of AA, BB and AB for EACH workload: 372 total independent child processes. No exclusions.
- Interleave six cells in a fixed seeded shuffled order per block; arm order alternates by block.
- Affinity mask 4 inherited by child; ABOVE_NORMAL priority, existing High Performance power plan.
- Run serially, with no build, GPU qualification, ASan or other performance task concurrently.
- Save stdout/stderr, exit status, command, timestamps, CPU times, working-set/page-fault/IO counters and
  binary hash for EVERY child. Retain raw intra-process timing arrays. Record power/CPU environment.
- Keep original 30/100 samples and original quantile convention. No timing amplification, changed Shader,
  changed quality, reduced operation count or production instrumentation.
- Stop after exactly 31 blocks, or immediately on a failed process / invariant / missing raw data.
  Interrupted execution is INCOMPLETE; do not restart until favorable. No adaptive extension this stage.

## Fixed statistics and decision

For every case and pair, report B/A minus one for process p50/p95/max, plus absolute values. Report the
median paired percentage and its 95% percentile bootstrap interval (20,000 resamples, fixed seed 20261012).
Report all per-process samples and distributions, not just pooled quantiles or means. No outlier removal.
Supplement with within-process ranges/IQR and between-process percentile ranges; pooled percentiles are
diagnostic only and never substitute for paired qualification.

Primary unresolved gates: C9_identity_and_candidate and complete_native_candidate_including_cook_io,
both p50 and p95. Closure requires paired median AND upper confidence bound <= +5%. AA and BB median
absolute differences must be <=5%, with intervals containing zero; otherwise measurement validity remains
PARTIAL. This adds uncertainty checks and does not waive the original 5% contract.

The same checks apply to whole Native Graph cold compilation, fixed Binding and warmed record/submit.
All other emitted metrics remain in the receipt, including adverse values. Bounded hot counters must be
zero. A regression with a confidence interval wholly above +5% is stable evidence for further attribution,
NOT authorization to change production. Any unresolved interval/invalid control means PARTIAL and STOP,
with possible follow-up experiments stated for user decision, not automatically run.

Explain C9 versus total using their aligned per-iteration samples and phase contributions. Quantiles are
not additive: the total p95 sample can be a different iteration from C9 p95. Identical object instruction
text does not prove final binary locality/cache equivalence. PMU attribution remains NOT_RUN unless a
real counter collection succeeds; no counter values are inferred from wall time.

## Functional qualification and exit

The existing indexed fixture gains UINT32 storage/type/range with actual indices 1,2,3 after two padding
elements. Reading these bytes as UINT16 would select a dummy vertex and fail the full red pixel oracle.
Retain UINT16, all four prior draw modes, two/three slots, negative cases and zero-allocation checks.
Add one CTest; the original 155 obligations remain. No public header or production change.

After doc/test/script Implementation I, use a clean `--no-hardlinks --no-checkout` clone at exact I.
Run tracked snapshot; Normal and Full ASan all builds twice; all 156 tests; validation/sync validation;
actual File API/source/include/link/codegen closure; source/user protection; readback and performance
evidence. Only then add a verification-only receipt V, ordinary fast-forward push and STOP.
An unresolved performance gate must remain PARTIAL despite passing GPU and functional tests.
