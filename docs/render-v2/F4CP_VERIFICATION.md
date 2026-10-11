# RD2-F4-C-P independent completion receipt

**PARTIAL — the bounded investigation is complete, but the cold performance gate is not closed.**
No production change or 5% waiver was made. The two new point estimates are below 5%, but their uncertainty
still includes unacceptable regressions. Stop after the preregistered experiment; no additional favorable
run is substituted. F4 stays PARTIAL. F5/F6/F7 remain NOT_AUTHORIZED.

## Identity, scope and independent qualification

- Approved entry V: `6342d56caf826504278399e2430a2a4499d9d3c2`.
- Previous production I: `5039fb42424dfda23bc7da518e7bbcf863f8c932`.
- This doc/test/script Implementation I: `92b126f67873ccbb576d0ffc5f1330964d6f84f6`.
- Independent clone: `D:/LuxQualification/render-v2-f4cp-92b126f67873/source`.
- `clone --no-hardlinks --no-checkout`, detached exact I, autocrlf=false, no alternates, clean tracked source.
  Both construction and independent clone pass ValidateTrackedSnapshot.cmake. No qualification source fix.
- This V adds only this receipt; its SHA is taken from the commit result, not recursively embedded here.
- Remote matched the entry at start and before I. Recheck again before ordinary fast-forward publication.

The six implementation paths are:

1. `modules/function/render/vulkan/test/NativeGraphDraw.cpp`
2. `cmake/render-v2-bootstrap/F4Closeout.cmake`
3. `cmake/render-v2-bootstrap/verify_f4.py`
4. `cmake/render-v2-bootstrap/measure_f4cp.py`
5. `docs/render-v2/F4CP_PROTOCOL.md`
6. `docs/render-v2/F4CP_TEST_IMPACT.md`

No production C++, compiler, owner, Shader, generator, architecture or public header changed. C1/C2/C3 and
all Draw production implementations remain frozen. No new API, owner, command system or future-stage type.

## Functional, GPU and protection results

| Gate | Result |
| --- | --- |
| Normal configure, all -j 4 -- -k 0 twice | PASS; second build no work |
| Full ASan configure, all twice | PASS; compatible SPIRV-Cross ASan dependency; second build no work |
| Normal CTest | 156/156 PASS, 45.93 s |
| Full ASan CTest | 156/156 PASS, 100.77 s |
| Original 155 test names/obligations | All retained; exactly one UINT32 addition |
| GPU validation and synchronization validation | 0 errors; known GalaxyOverlay naming warnings retained |
| Existing W02/W03/W04/W16 independent readback oracle | 45 binary readbacks in each build; max absolute error 3.973642981325298e-08; native/separate multiview equal |
| Actual dependency closure, public headers, compile/link/codegen | PASS in both builds; 358 compiler headers and 182 generated artifacts each |
| Production/source/Shader/generator identity | Unchanged Git objects; no hidden qualification input |
| Frozen Legacy | 719/719 blob/mode/size match; guard is the 720th tree item |
| Core/Transport/F2/R4 and FINAL/RD2/historical reports | Unchanged |
| Original six user edits and V2 Operation.hpp | HEAD/branch/status/staged/unstaged/untracked and all seven byte hashes unchanged |
| Three installed public header prefixes | Still exact locked public-header bytes; no synchronization change needed |
| Historical performance evidence | All 540 indexed files rehashed unchanged, including unfavorable samples |

The new `render.vulkan.closeout_draw_indexed-u32` uses the existing F4CIndexed generated Params and Shader
pair. A real INDEX buffer contains UINT32 values 1,2,3 at byte offset 8, with declared byte range [8,20).
It skips the existing unusable dummy vertex. The actual native indexed draw produces every expected red
RGBA pixel. Interpreting those bytes as UINT16 would select different indices; this is not a type-only test.
Existing wrong-use/offset/type/count/stride checks remain. Both two/three frame slots complete 27 submissions,
including 24 warmed zero-allocation/zero-byte submissions each, with vertex/index Sync2 evidence.
The UINT32 logs and assertions are separate from the 45 archived W-workload readbacks counted above.

Device: RTX 4070 Ti, driver raw 2480242688, API 4211013 (1.4.325); graphics/compute/transfer families 0/2/1.
MSVC 19.44.35228 x64 RelWithDebInfo, Vulkan SDK 1.4.304.0, Ninja. CPU: i7-13700KF, 24 logical processors.
GPU performance measurement disables validation as before; correctness runs enable validation/sync validation.

## Frozen experiment and measurement identity

The protocol was committed in I before any new AA/BB/AB observation. It fixed 31 pairs per cell, two
unchanged complete workloads, seeded interleaving, alternating arm order, affinity mask 4 and ABOVE_NORMAL
priority under the existing High Performance plan. All 372 independent processes completed once. None
were discarded/replaced; no retry or extension occurred. Raw rows total 2,604, including 5,580 C9 timings
and 18,600 complete-candidate timings across AA/BB/AB. Every reported process quantile was recomputed from
the archived raw array. The original 30/100 samples per process and quantile convention were retained.

A(Graph) is locked original F4 I `bd5399063907087e147ee159f24e1da6a87ab30f`. A(Shader) is the archived
original F3 `0fbbbd6db3aa4ad53283522e4a54d96556b5b478` same-object finalized `/INCREMENTAL:NO` executable.
B is this independent I build. Original-link F3 results remain preserved, not substituted or erased.
Actual Ninja link commands and prior relink input hashes are retained: Graph uses its original incremental
link policy; Shader uses the established finalized policy on both compared arms.

All 252 compile commands match F4-C after replacing only exact clone/output root prefixes. Both new
benchmark `.text` section hashes exactly equal the corresponding locked F4-C benchmark `.text` hashes.
Whole EXE/object hashes differ and are recorded; no claim of whole-file binary identity is made. The C9
entry/constructor/timer disassembly also matches original F4 and F4-C: 568/138/3/13 instructions respectively.
These observations exclude an unrecorded algorithm/compiler-option change, not all linker/data locality effects.

All five consumed SPIR-V binaries match the corresponding A inputs byte-for-byte. The four historical F3
source files use CRLF versus current LF, totaling 34 extra bytes; normalized content is identical. This
inherited input detail is retained explicitly, not silently rewritten or asserted to explain the regression.

QPC frequency is 10 MHz; recorded timings have 100 ns granularity. A tick is approximately 1.67% of a 6 us
C9 observation, but only 0.004% of a 2.5 ms candidate. C9 process p95 is the 29th of 30 ordered samples.
Complete candidate includes six file reads, variant/reflection validation, logical fixture compilation,
the existing rejected format check and accepted native program creation. Native/OS caches are warmed and
uncontrolled; cold means new objects, not erased driver/file caches. Pipeline creation uses no explicit
VkPipelineCache. File IO counters, CPU times, page faults/working sets and wall intervals are archived per
process. They include process startup and all cases, so they cannot isolate the timed candidate's IO cost.
Shader-process median read operations are 11,528 for A and 11,663 for B; occasional extra reads are retained.
Reported CIM clock is not a measured per-sample frequency. No PMU/cache-miss/instruction-counter claim is made.

## Performance result, without sample selection

Percentages below are medians of paired per-process B/A differences. Intervals are fixed-seed 20,000-resample
95% bootstrap intervals. The preregistered rule requires median and upper bound <=5%, plus valid AA/BB
controls; an improved point estimate alone is insufficient.

| Metric | p50 % [95% CI] | p95 % [95% CI] | Current conclusion |
| --- | --- | --- | --- |
| C9 identity/candidate | +2.0408 [0, +4.0816] | +3.4483 [-1.4085, +12.0690] | PARTIAL |
| Complete candidate including cook IO | +1.1570 [+0.6014, +2.1679] | +2.9952 [+0.3965, +6.8838] | PARTIAL |
| Whole Native Graph / prebuilt programs | +0.8163 [-0.8989, +3.0374] | +1.2618 [-8.4986, +8.0925] | Tail uncertainty retained |
| Fixed Binding | -2.5000 [-2.5000, -2.4793] | 0 [-0.7813, +0.7937] | PASS; zero allocation |
| Warmed Native record/submit | +3.0534 [+0.3817, +3.4749] | +3.7479 [-4.1935, +11.1332] | Unresolved tail/control; zero allocation |
| C10 native resources | -1.2987 [-3.2258, +2.0270] | +6.6351 [-3.6697, +9.4787] | Adverse observation retained |
| C11 recipes/descriptors | +2.3810 [+0.9174, +5.5556] | +8.6777 [+0.3333, +17.1429] | Adverse observation retained |
| C12/C13 sync/admission | -6.2500 [-8.8235, -3.3333] | -12.1951 [-16.6667, -4.8780] | Favorable observation, not a substitute for other gates |

All other metrics, every paired difference, within-process range/IQR and process distribution are in
`performance/comparison.json` and `performance/rows.jsonl`; no GPU timing, layout, relocation, descriptor
or native pipeline sample is omitted. Per-process bounds, process counters and phase alignment are in
`performance-audit.json`.

| Same-binary p95 control | Paired median % | 95% CI % |
| --- | ---: | --- |
| C9 AA | -1.6949 | [-5.7971, +5.1724] |
| C9 BB | -1.6667 | [-4.9180, +3.3333] |
| Complete candidate AA | -0.0535 | [-2.8178, +3.3764] |
| Complete candidate BB | +0.7802 | [-6.5414, +5.0772] |
| Warmed record/submit BB | +5.6285 | [-7.7061, +9.4891] |

The historic +6.5574%/+5.9257% are not overwritten. Increasing independent replication narrowed the
complete-candidate interval, but still did not exclude a >5% tail regression. C9 quantization and sparse
tails remain limiting. Neither primary interval lies wholly above +5%, so this does not establish a stable
unacceptable production regression either. Same-binary variability is demonstrated; it does not prove
that every A/B difference is environmental. No specific production hotspot has sufficient causal evidence
to authorize a fix, and no production fix was attempted.

C9's median share of total native compile is 11.35% in this AB sample (historically 11.58%). Only 16 of 62
processes have the same C9-p95 and total-p95 iteration (historically 4/18). Other phases can dominate or move
in opposite directions, and quantiles cannot be added. This explains why whole compile and C9 estimates
need not have the same direction; it does not waive the local gate.

## Objective limits and choices for further authorization

The fixed 31-block experiment is finished. Do not continue sampling under this authorization. The current
timer/sample structure and uncontrolled driver/file/scheduling behavior cannot bound the two tails tightly
enough. No new change to Plan.cpp, Graph versions, Shader generation or owners is justified by these data.

Possible user decisions, none implemented here:

1. Authorize a separately preregistered diagnostic build of BOTH locked variants that increases within-process
   cold samples and isolates file/reflect/native segments while keeping the complete operation as the gate.
   This needs new matched baseline measurements; it cannot reuse old A percentages against new B.
2. Supply a controlled profiling environment with scheduler/frequency/PMU access and defined cache policy,
   then approve a bounded experiment aimed at causality. No claim that such access was available here.
3. Explicitly accept a specified uncertainty/correctness cost after review. No such waiver is assumed.

Target-relative extent, Cube, 3D and IndirectCount retain the prior user-approved future responsibilities;
they remain NOT_IMPLEMENTED, not hardware fallbacks. Product/installed SDK/Linux/Android/Runtime/Feature
qualification is NOT_RUN. F4 and all future stage authorizations remain unchanged by passing this test addition.

## Evidence receipt

`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F4-C-P/92b126f67873ccbb576d0ffc5f1330964d6f84f6/`

`evidence-manifest.json`: 2,655 files, SHA-256
`737a6f3c9afe13f03d6bf9577914fe1d1e8b00a264cbaf631b09471b2d4f582f`.

Includes the exact protocol/script identities, all commands/exit codes, original evidence index, independent
Normal/ASan builds and tests, actual dependency closure, binaries/objects/inputs, GPU traces and readbacks,
all performance pairs/controls and process counters, source/user/install protection, and numerical audit.
The clean source remains at I. This report is the only V addition.

```text
RD2_F4C_P = PARTIAL
UINT32_NATIVE_INDEXED_DRAW = PASS
NORMAL = 156/156 PASS
FULL_ASAN = 156/156 PASS
GPU_VALIDATION_SYNCVALIDATION = 0 ERROR
FIXED_BINDING = PASS
COLD_P95_CLOSEOUT = PARTIAL
PRODUCTION_CHANGES = NONE
F4 = PARTIAL
F5_F6_F7 = NOT_AUTHORIZED
V2_PRODUCT = EXPECTED_UNAVAILABLE
STOP
```
