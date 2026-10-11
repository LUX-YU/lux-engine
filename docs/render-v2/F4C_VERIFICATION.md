# RD2-F4-C independent verification

**Overall: PARTIAL.** Native correctness, authorized draw primitives, Normal/Full ASan/GPU/dependency gates
and the original fixed Binding gate pass. Two cold p95 comparisons remain unresolved; no performance waiver
is taken. F4 remains PARTIAL and F5/F6/F7 remain NOT_AUTHORIZED.

## Locked identity and isolation

- Entry / approved RD2 V: `6b777c96bbe3986c940cafe4a3e213ae2e0a3194`.
- Implementation I: `5039fb42424dfda23bc7da518e7bbcf863f8c932` (33 changed files, listed below).
- Frozen Render V1: `a669409a289a6fa4092f21176397795b1cdb7f3e`.
- Previous F4 I/V: `bd5399063907087e147ee159f24e1da6a87ab30f` / `b9f9a909bc34ac87f00c594996201a20f0af00c8`.
- Independent source: `D:/LuxQualification/render-v2-f4c-5039fb42424d/source`.
- Clone command uses `--no-hardlinks --no-checkout`; checkout is detached at I, core.autocrlf=false,
  no alternates, clean tracked source. Both construction and clone pass ValidateTrackedSnapshot.cmake.
- Normal and ASan builds are outside source. Qualification made no source fixes. This verification commit
  adds only this report; its exact SHA is obtained from Git after committing, not embedded recursively.
- Remote was rechecked at the approved entry before implementation and before this receipt/push.

## Real reproduction and correction

| Item | Original observation | Locked-I result |
| --- | --- | --- |
| C1 compute readback | Real byte oracle passed, but trace lacked SHADER_STORAGE_WRITE -> HOST_READ; dedicated transfer/compute queues also produced VUID-VkBufferMemoryBarrier2-srcStageMask-03851 after HOST contaminated GPU last-access state | PASS. Cold recipes resolve output ranges and actual possible last writers. Record emits Sync2 host publication and preserves GPU last-access/family. Single/multiple queues, partial ranges, ready/BUSY and numerical readback pass |
| C2 imported memory alias | Two actual Image::alias images had different handles on overlapping allocation storage; accepted execution produced SYNC-HAZARD-WRITE-AFTER-WRITE | PASS. Existing VMA allocation facts reject unproven physical overlap, including a different aliased handle overlapping an in-flight previous slot even when a ready ticket exists |
| C3 A -> B lifetime | No failure reproduced in the original real GPU case | PASS regression / original risk NOT_REPRODUCED. Expanded case destroys heap-allocated A owner and external view while a completed old slot still retains its borrow, then submits B and destroys plan; Normal and Full ASan pass. No speculative ownership rewrite |

C1 does not claim that the original observed readback bytes were wrong. C2 is an actual validation failure,
not merely a source-level suspicion. Failed baseline executable/libraries, traces and logs remain in
`../baseline-reproduction/`. The partial-export fixture also reproduced admission of undeclared readback
bytes before adding the explicit exported-range check.

BufferMemoryBinding is only a value snapshot from VMA, not an owner. Buffer storage layout is unchanged.
No new allocator, queue, registry, Runtime, DataStorage, Feature interface or retirement hierarchy exists.
Native GPU completion still comes from the existing queue ticket; CPU dirty state is not used as completion.
HOST_READBACK's existing staging barrier remains intact. Normal teardown uses existing last-use retirement.

## User-approved bounded draw extension

The §G decision and precise allowed-file impacts were recorded in F4C_SCOPE_AUDIT.md after preserving the
pre-draw correctness and performance evidence. Implemented through existing Graph uses, F3 pipelines and
F4 Native recipes:

- Vertex streams refer to validated VERTEX use indices. Pipeline VertexBinding remains the sole stride source.
- UINT16/UINT32 IndexStream and direct indexed draw refer to INDEX uses; actual indexed GPU fixtures use UINT16.
- Ordinary and indexed indirect commands refer to INDIRECT uses. One command per recipe; no MultiDraw scheduler.
- Real compute shader writes argument bytes, graphics consumes them without CPU argument readback.
- Transfer/compute/graphics families 1/2/0 and Sync2 traces prove STORAGE_WRITE -> INDIRECT_READ and vertex/index access.
- Vertex, indexed, indirect and indexed-indirect fixtures produce the expected full-screen red triangle pixels.
  The indexed fixture skips a deliberately unusable dummy vertex via its actual index buffer.
- Wrong resource kind/use, absent use, native usage, offset/range, stride, unsupported index type, direct count,
  indirect storage/count, physical vertex-stride limit and invocation dynamic offset are rejected before unsafe access.
- Two and three frame slots each complete 27 submissions, with 24 warmed samples: 0 first-party C++ allocations
  and 0 bytes during submit. Cold creation and hot p50/p95/max are in the raw logs. Record/prepare source has no
  lazy native allocation/creation path; the actual hot link map excludes cold Shader compiler dependencies.

CPU admission validates known arguments and storage. GPU index/argument contents remain the producer's
bounded-content contract; firstInstance must be zero for indirect draws because that feature is not enabled.
No CPU readback is introduced to inspect GPU arguments. Native footprint support and explicit UNSUPPORTED
results are documented in F4C_TEST_IMPACT.md; this is not a claim to implement all future Mesh algorithms.

## Qualification results

| Gate | Result and evidence |
| --- | --- |
| Normal bootstrap configure + all -j 4 -- -k 0 | PASS; second build reports no work |
| Full ASan configure + two all builds | PASS; compatible SPIRV-Cross ASan prefix; second build no work |
| Original 146 obligations plus 9 additions | Normal 155/155 PASS (43.45 s); Full ASan 155/155 PASS (99.11 s) |
| Foundation ownership/fault, Shader/descriptor, Native import/alias/multiqueue/retirement regressions | PASS as part of both complete matrices |
| Real validation and synchronization validation | 0 errors; known GalaxyOverlay layer naming warnings retained, not discarded |
| Original W02/W03/W04/W16 numerical readback audit | 45 archived binary readbacks per build; independent oracle max absolute error 3.973642981325298e-08; native/separate multiview outputs equal |
| Actual File API/source/include/link/Ninja/codegen closure | PASS in both builds; 358 headers and 182 generated artifacts each |
| Independent public headers and dependency negative tests | PASS, retained in complete CTest inventories |
| Core/Transport/F2/Description/generator/FINAL/RD2/historical reports | Exact Git objects unchanged |
| Frozen Legacy | 719/719 blob/mode/size match; archive guard is the additional 720th item |
| User workspaces | Original six edits and V2 Operation.hpp byte hashes, HEAD/branch/status/staged/unstaged/untracked snapshots unchanged |
| Three installed public header prefixes | Exact I LF bytes synchronized for Memory.hpp and Executable.hpp; separate receipt; no installed-SDK/Android qualification claim |

New CTest names are `render.vulkan.closeout_C1`, `_C1-multi`, `_C1-partial`, `_C2`, `_C3`,
`render.vulkan.closeout_draw_vertex`, `_draw_indexed`, `_draw_indirect`, `_draw_indexed-indirect`.
The full exact names and retained original names are in `closure/test-obligations.json`.

GPU: NVIDIA GeForce RTX 4070 Ti, raw driver version 2480242688, API 4211013 (1.4.325), Vulkan SDK 1.4.304.0,
MSVC 19.44.35228 x64 RelWithDebInfo/C++20, Ninja. Device extension/feature setup follows the existing explicit
VulkanDevice options; validation uses the actual KHRONOS layer plus synchronization validation. Raw device,
queue, memory flags and extension/capability evidence remains in test and prior explicitly referenced logs.
No host-visible noncoherent memory type exists on this device: forced noncoherent GPU path is NOT_RUN;
the real Buffer::read/invalidate path runs, and existing ownership/fault tests remain covered.

## Performance: original Binding gate closed; cold uncertainty retained

Every comparison uses nine independent-process pairs, alternating AB/BA, CPU affinity mask 4,
ABOVE_NORMAL priority, High Performance power plan, unchanged workloads. No samples excluded.
Confidence intervals use 10,000 fixed-seed bootstrap medians of paired percentages.

| Workload / baseline | p50 paired median | p95 paired median | Result |
| --- | ---: | ---: | --- |
| Fixed Binding / original locked F3 binary | -2.5000% | -0.7752% | PASS; 0 allocations/bytes |
| Fixed Binding / same original F3 objects finalized with /INCREMENTAL:NO | -2.5000% | -0.7752% | PASS; 0 allocations/bytes |
| Native Graph warmed record/submit / original F4 | +0.3846% | -2.6866% | PASS; 0 allocations/bytes |
| Native Graph whole compile with prebuilt programs / original F4 | -1.5493% | -7.6190% | PASS |
| C9 identity/candidate subphase / original F4 | +4.1667% | **+6.5574%** | Unresolved; p95 CI [0%, +13.7931%] |
| Complete native candidate including cook I/O / finalized F3 control | +1.6032% | **+5.9257%** | Unresolved; p95 CI [-5.7672%, +14.6731%] |

The original F3 full Foundation/Shader comparison has no >5% p50/p95 result; its complete candidate p95
is -3.993%. Original F4 +5.0420% Binding and the pre-fix +5.6911% reproduction are retained unchanged.
Same-object relink controls, ILT call sites, layout/obj/compile diagnostics support the minimal finalized
benchmark link change, without changing Bind production code, workload or normal validation.

Additional fixed AA/BB cold controls were run once, with no replacement AB run:
- Complete-candidate p95 median AA -2.4015%, BB +0.1898%; intervals [-12.1321%, +12.2668%] and [-9.6766%, +10.7429%].
- C9 p95 median AA 0%, BB 0%; intervals [-18.6667%, +10%] and [-18.2927%, +22.5806%].
- Other control metrics also fluctuate above 5% (AA GPU timestamp; BB C10 p95); all remain visible in JSON.
- Object disassembly for compileVulkanGraph (568 instructions), NativeGraphBacking constructor (138),
  and timer helpers is textually identical after omitting raw encoding in both versions. Object hashes differ;
  this comparison does not prove identical final link locality or establish that all timing changes are noise.

These controls expose substantial cold-tail uncertainty; they do not prove a new resource-version algorithm
regression. They also do not establish a causal explanation/equivalence bound sufficient to waive the two
observed >5% AB p95 values. Therefore overall qualification remains PARTIAL. No further production change,
favorable-sample selection or relaxed gate was used. WPR PMU capture was denied (0x80070005); counters are NOT_RUN.

## Remaining responsibilities and stage entry

1. Resolve the two cold p95 comparisons through a bounded attribution/measurement plan, retaining all samples,
   or obtain an explicit user decision about their acceptance. Only then can this receipt be supplemented as PASS.
2. Target-relative extent remains NOT_IMPLEMENTED for F5 real Target Signature/resize/storage/cache.
3. Cube remains NOT_IMPLEMENTED for F9 Skybox/first consumer; 3D for the first volume/GI consumer;
   IndirectCount no later than F8. These are explicit user deferrals, not device fallbacks or feature deletion.
4. F5/F6 must settle public Feature API dependency direction and plugin codegen timing; shared-backing mechanism
   proof in F5 does not replace the real DataSet/publication chain required in F7.
5. F5 still requires independent user authorization after F4 closure review. No Runtime/Scene/Feature SDK,
   DataSchema/TypedStorage or future frame loop has been implemented here.

Product builds, Linux, Android, complete installed SDK and new Runtime/advanced rendering qualification are
NOT_RUN; product remains EXPECTED_UNAVAILABLE. Historical F4 PARTIAL and all earlier results remain unedited.

## Evidence and exact change list

Evidence directory:
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F4-C/5039fb42424dfda23bc7da518e7bbcf863f8c932/`

`evidence-manifest.json` covers 2209 files; SHA-256:
`612b0b99cb69fbd3748f6437bade29101a84911503108baf556ec19493dfea35`.

It includes commands/exit codes, both builds/tests, actual closure, generated sources/SPIR-V/reflection,
binary identities, raw readbacks/oracles, validation traces, performance pairs/controls, cold code comparison,
user protection and header sync receipts. `development-evidence-index.json` anchors the original reproduction,
pre-draw preservation, original failed samples, machine-code investigation and same-object relink controls
outside this I directory. Failed diagnostic/build attempts remain archived. Evidence collection does not
change the clean qualification source or rewrite old reports.

Implementation paths (Git blobs and per-file SHA-256 in `changed-files.json`):

- `cmake/render-v2-bootstrap/CMakeLists.txt`
- `cmake/render-v2-bootstrap/F4Closeout.cmake`
- `cmake/render-v2-bootstrap/verify_f4.py`
- `docs/render-v2/F4C_PERFORMANCE_INVESTIGATION.md`
- `docs/render-v2/F4C_SCOPE_AUDIT.md`
- `docs/render-v2/F4C_TEST_IMPACT.md`
- `docs/render-v2/F4C_WORK_ORDER.md`
- `modules/function/render/vulkan/include/lux/engine/render/vulkan/graph/Executable.hpp`
- `modules/function/render/vulkan/include/lux/engine/render/vulkan/memory/Memory.hpp`
- `modules/function/render/vulkan/pinclude/GraphNative.hpp`
- `modules/function/render/vulkan/src/graph/Frame.cpp`
- `modules/function/render/vulkan/src/graph/Recipes.cpp`
- `modules/function/render/vulkan/src/graph/Record.cpp`
- `modules/function/render/vulkan/src/graph/Sync.cpp`
- `modules/function/render/vulkan/src/memory/Memory.cpp`
- `modules/function/render/vulkan/test/NativeGraphCloseout.cpp`
- `modules/function/render/vulkan/test/NativeGraphDraw.cpp`
- `modules/function/render/vulkan/test/shader/F4CArguments.comp.lglsl`
- `modules/function/render/vulkan/test/shader/F4CArguments.hpp`
- `modules/function/render/vulkan/test/shader/F4CIndexed.frag.lglsl`
- `modules/function/render/vulkan/test/shader/F4CIndexed.hpp`
- `modules/function/render/vulkan/test/shader/F4CIndexed.vert.lglsl`
- `modules/function/render/vulkan/test/shader/F4CIndexedIndirect.frag.lglsl`
- `modules/function/render/vulkan/test/shader/F4CIndexedIndirect.hpp`
- `modules/function/render/vulkan/test/shader/F4CIndexedIndirect.vert.lglsl`
- `modules/function/render/vulkan/test/shader/F4CIndirect.frag.lglsl`
- `modules/function/render/vulkan/test/shader/F4CIndirect.hpp`
- `modules/function/render/vulkan/test/shader/F4CIndirect.vert.lglsl`
- `modules/function/render/vulkan/test/shader/F4CVertex.frag.lglsl`
- `modules/function/render/vulkan/test/shader/F4CVertex.hpp`
- `modules/function/render/vulkan/test/shader/F4CVertex.vert.lglsl`
- `modules/function/render/vulkan/test/shader/F4CWrite.comp.lglsl`
- `modules/function/render/vulkan/test/shader/F4CWrite.hpp`

```text
RD2_F4C = PARTIAL
C1_C2_CORRECTNESS = PASS
C3_ORIGINAL_BUG = NOT_REPRODUCED
AUTHORIZED_NATIVE_DRAW = PASS
NORMAL = 155/155 PASS
FULL_ASAN = 155/155 PASS
GPU_VALIDATION_SYNCVALIDATION = 0 ERROR
P1_FIXED_BINDING = PASS
COLD_P95_CLOSEOUT = PARTIAL
F4 = PARTIAL
F5_F6_F7 = NOT_AUTHORIZED
V2_PRODUCT = EXPECTED_UNAVAILABLE
STOP
```
