# F4 independent qualification — PARTIAL

**F4 = PARTIAL. F5 = NOT_AUTHORIZED. STOP.**
Native Graph functionality, normal/full-ASan integration and bounded allocation checks passed in the tested
scope. The comparable F3 fixed-binding performance gate remains unresolved; this report does **not** waive
it or declare the complete F4 stage PASS. Unsupported implementation paths below are not device fallbacks.

## Identity and isolated procedure

| Item | Identity |
|---|---|
| Approved entry / remote before publication | `25bd4e16358bfe3a8c1163a18b17b807bd6a5505` |
| First implementation, retained with failed performance evidence | `6556c20c9d0a6ea28ba05628e84e7476a9afd61b` |
| Final implementation I | `bd5399063907087e147ee159f24e1da6a87ab30f` |
| Verification V | The commit adding **only this file**; no production changes |
| Locked F3 comparison implementation | `0fbbbd6db3aa4ad53283522e4a54d96556b5b478` |
| Frozen V1 | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| Independent source | `D:/LuxQualification/render-v2-f4-bd5399063907/source` |
| Independent normal / ASan output | Sibling `build` / `asan` directories |
| Evidence archive | `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F4/bd5399063907087e147ee159f24e1da6a87ab30f/` |
| Evidence manifest SHA-256 | `6bf958f73e875b249ecd19fede44d001a1e6e5e6d72ed0a9dfc5e82cc5f3b756` |

The 1,768-file evidence manifest seals commands, exit codes, inputs, generated outputs, binary identities,
raw GPU data, samples and protection receipts. `setup.json`, `identity.json`, `run.py` and `qualify.py`
record the reproducible invocation. Both construction and independent checkout passed
`ValidateTrackedSnapshot.cmake`; clone used `--no-hardlinks --no-checkout`, then detached checkout of I.
No hidden patch was applied to the qualified clone. This is an isolated qualification process, not a claim
that an additional human reviewer independently reviewed the implementation.

The first I passed 146/146 normal and 146/146 full-ASan tests but failed performance. Its archive and all
unfavorable development samples remain intact and are anchored by the final archive's evidence indices.
Production corrections went into the new I before its fresh clone and complete qualification. No test was
removed, renamed away from a failure, or weakened to obtain these results.

## Actual implementation and ownership

[Implementation / type and algorithm inventory](F4_IMPLEMENTATION.md),
[authorized consumer and conditional extension inventory](F4_WORK_ORDER.md), and
[test impact policy](F4_TEST_IMPACT.md) describe the final code. Exact changed Git blobs and actual compiled
relationships are additionally in `test-impact.json` and both `closure/closure.json` reports.

- `compileVulkanGraph()` is a free cold algorithm. It consumes the existing F2 plan and complete F3
  candidates, builds native backing, recipes and Sync2/queue dependencies, then returns one move-only
  `ExecutableGraphPlan`. It does not implement a second logical scheduler, reflection system or Runtime.
- The plan copies its logical facts and owns F3 pipeline/layout candidates plus per-slot R4 Buffer/Image,
  ImageView and descriptor owners. Device/allocator/queue owners outlive it. Views/descriptors are released
  before resource backing; alias images die before their allocation donor. No second VMA owner exists.
- Imports are synchronous explicit borrows, with exact native shape/range/initial state and real admitted
  queue tickets. Prepared views may change only on a completed slot. Exported backing remains borrowed;
  external users return last-use evidence before slot reuse/retirement. Initial external barrier state is
  a caller contract, not something a nonzero handle or fence alone can prove.
- The existing SubmissionQueue now supports actual queue roles, semaphore waits and family ownership
  transfers. Same-family distinct queues still synchronize. Final joined tickets cover all participating
  queues. Accepted partial submissions pin all backing; device-lost errors preserve VkResult and terminate
  subsequent submission. Normal resource retirement does not use DeviceWaitIdle.
- F3 binding/layout ownership is reused. Immutable descriptor creation is the default; only Native Graph
  explicitly requests `COMPLETED_ONLY` rewrite recipes. All old-use receipts must complete before rewrite;
  all values are validated before the first write. Immutable/busy/foreign/shape negatives pass.
- Single-queue, distinct-family queues and three distinct queues in one family all ran. Conservative
  alias eligibility proves same actual queue, disjoint uses, compatible memory requirements/shape/usage,
  nonpersistent lifetime and independent FIF allocation. Ineligible cases really allocate separately.

C9–C13 are implemented and exercised for the native paths below. This is not an assertion that every future
Feature, resource shape or graphics draw form is implemented. The current native recorder accepts absolute
2D images, arrays/mips/samples, direct procedural draws, compute and copy/readback. **3D/cube/relative extents
and vertex-stream graphics remain NOT_IMPLEMENTED and return structured errors.** These are implementation
limits, not unsupported hardware. This report does not unilaterally remove or reassign any FINAL obligation.
Local same-attachment reads do not claim overlapping-fragment rasterization-order access or OIT correctness.

## Independent gates and actual GPU evidence

| Gate | Final I result / raw evidence |
|---|---|
| Normal configure / all build twice | PASS; `build-1.log`, `build-2.log`; second says `ninja: no work to do` |
| Normal CTest | **146/146 PASS**, 37.08 s; `test.log` and archived LastTest.log |
| Complete ASan configure / all build twice | PASS; all compiler TUs carry `/fsanitize=address`; compatible SPIRV-Cross ABI; no annotations disabled |
| Complete ASan CTest | **146/146 PASS**, 75.07 s; `asan-test.log` |
| Original 137 obligations | All retained and executed, including F2 oracle/bindings and R4/F3 GPU regressions |
| New obligations | Native graph, graph candidate, graph benchmark; six actual native include rejection probes |
| Public headers / negative compilation | PASS via actual built TUs and CTest; no header-only grep substitute |
| Validation / SyncValidation | **0 errors**, normal and ASan GPU fixtures; raw callbacks retained |
| Golden readback secondary oracle | **45 raw samples per configuration**, all pass offline Python oracle; max absolute error `3.973642981325298e-08`; native/separate Multiview bytes equal |
| Cold failure and lifetime | 12 new graph fault points; last-good, second-slot rollback, immutable/busy/foreign/shape errors, held completion, retirement capacity and terminal failure pass |
| Existing R4 fault obligations | 28 fault points / 13 owners pass |
| Steady native creation | Candidate fixture records `hot_native_creations=0` |
| Steady first-party C++ allocation | **0 count / 0 bytes** for real warmed Graph record/submit; existing bounded mechanisms and binding also zero |
| Source/include/link/codegen closure | PASS, normal and ASan; 346 actual headers, 144 explicitly checked generated artifacts; complete generated trees also archived |
| Comparable performance gate | **UNRESOLVED / stage blocking**, detailed below |

Device: **NVIDIA GeForce RTX 4070 Ti**, vendor 4318, device 10114, driver raw `2480242688`
(NVIDIA 591.86.0.0), physical API **1.4.325**; application requests Vulkan 1.3.
Actual queue topology: family 0 graphics/compute/transfer (16 queues), family 1 transfer (2),
family 2 compute/transfer (8). Tests used families 0/2/1 and separately family 0 indices 0/1/2.
Synchronization2 is enabled; dynamic rendering, timeline semaphores for multiple queues, Multiview,
separate depth/stencil layouts and `VK_KHR_dynamic_rendering_local_read` are explicitly negotiated and
used by their corresponding fixtures. `VK_LAYER_KHRONOS_validation`, debug utils and synchronization
validation are active. Native graph reports 57 non-error GalaxyOverlay layer-name warnings across its
instances; these are retained, not relabeled as zero warnings. F4 native local-read and Multiview paths
actually ran on this GPU, as did their explicit fallback paths.

| Original workload / capability | Final I sub-evidence; does not close the blocked stage |
|---|---|
| W02 / I03–I04 | One VkImage, 4 mips, 2 layers, repeated real downsample/readback vs CPU oracle; actual range barriers; intermediate READ_WRITE versions with RAW/WAR/WAW |
| W03 / I03–I04 | Compute→graphics→readback; single, distinct-family, same-family distinct queues; image/buffer copies, offsets 16/32, two mip copy, export into another graph; wrong ticket/family/range readiness rejection |
| W04 / I05 | Alias/no-alias same output; 3 FIF; actual VkDeviceMemory/offset receipts: **18→12 allocations**, **7680→4608 VMA allocation bytes**, six aliases; overlap/multi-queue/incompatible-shape cases produce zero aliases |
| W16 / I35 | Real layered Multiview and two separate draws, distinct per-layer values, exact output equality; no Scene/View Runtime claim |
| I36 / I37 | Conditional skip selects valid current fallback over 8 frames; imported backing/view and history epoch; MSAA4 resolve, depth/stencil and color clear/load/store; native input attachment and same-attachment read vs explicit sampled fallback |
| I13 / I14 | Existing staging/submission/retirement used by Graph; export handoff, bounded busy/capacity, partial candidate cleanup, 2/3 FIF and all-queue completion before recycle |
| I44 | Full real 256×256 compute/graphics/copy workload, zero warmed first-party allocation and no lazy native construction; legacy bind speed gate remains open |
| REQ19–24 / 38 / 46 | Native mechanism, dependency, ownership and fallback sub-evidence above; no blanket cross-stage completion declaration |

`artifacts-{build,asan}/native-evidence/` contains 45 `.bin` readbacks each, their hashes, memory allocation
receipts, range/queue/barrier traces and benchmark samples. `verify-readbacks.py` independently checks
those recorded bytes; `readback-oracle.json` records the results. `test.log` also retains direct production
trace JSON linked to F2 resource versions/writers/readers. W04 numbers are actual VMA allocation sizes,
**not** whole-process VRAM budget or driver heap reservation measurements.

## Performance: remaining stop gate

Unchanged benchmark source Git blobs and identical F3 SPIR-V are proven in
`benchmark-workload-identity.json`. Comparison uses locked F3 binaries against final I, independent
processes, affinity mask 4, ABOVE_NORMAL priority, nine alternating AB/BA pairs, no discarded samples.
`native-performance/comparison.json` contains each paired difference, p50/p95/max, raw Shader samples,
10,000-resample paired bootstrap intervals and executable hashes. The old Foundation executable only
emits per-process quantiles; no nonexistent intra-run samples are claimed. New Graph has no equivalent
old Graph workload, so its measurements are a baseline, not an improvement percentage.

| Unchanged workload | Paired median p50 difference | Paired median p95 difference |
|---|---:|---:|
| `buffer_1MiB_create_destroy_cold` | +0.0000% | +0.0000% |
| `complete_descriptor_pool_sets` | +0.0000% | +0.0000% |
| `complete_native_candidate_including_cook_io` | +0.3978% | -6.9792% |
| `fixed_native_binding_recipe` | +5.0420% | +4.7619% |
| `gpu_shared_2x2_draw_barriers_readback_timestamp` | +0.0000% | -0.7143% |
| `layout_full_shape` | +0.0000% | +12.5000% |
| `native_end_reset_submit` | -1.2500% | -0.8130% |
| `native_graphics_pipeline_create_destroy` | +0.0000% | +0.0000% |
| `relocation_validation_vertex` | +0.2145% | +0.7997% |
| `retire_collect_native_buffer` | +0.0000% | +0.0000% |
| `staging_1MiB` | +0.3876% | +2.2508% |
| `transfer_1MiB_roundtrip` | +0.6221% | -1.6575% |

The initial I's descriptor creation cost was real: unconditional rewrite metadata changed 100 immutable
creations from 200 allocations / 1,600 bytes to 600 / 105,600. The minimal update policy and reserved recipe
storage restore the old immutable allocation census and **0% / 0% paired median** creation difference.
Rewritable Native Graph descriptors still own all required validation facts; no hot allocation or unsafe
rewrite was substituted.

Fixed binding remains unresolved: final paired p50 **+5.0420168067%**, 95% interval
**[+3.3058%, +7.6271%]**; paired p95 **+4.7619%**. Per-process p50 medians are 11.9 ns (A) and 12.6 ns (B).
Initial I measured +8.4746%/+9.6774%; the corrected development binary measured +6.7227%/+6.4516%.
Initial A/A and B/B bind controls were approximately +0.85%/+0.80% and -0.78%/-0.73%.
The initial A/B bind function disassembly was byte-identical (SHA-256
`fea0a265143cee7fcd05ff504d5b1c78bee8fe1b1583e40c7b873caa6db99b7a`). This does **not** prove identical caller,
whole-binary layout or driver behavior. A specific cause has not been established; no correctness-cost
exception has been requested or accepted. The final interval crosses the threshold, so it also does not
justify claiming a precisely known >5% production cost; the combined evidence fails to establish clearance.

The Layout p95 +12.5% result is measurement-limited: old A/A produced the same +12.5% paired median,
interval spans zero, and A/B per-process p95 medians are both 800 ns with 100 ns quantization. This is
reported separately and cannot be used to dismiss the binding result. Full investigation and all runs
remain in `performance-investigation.json` and referenced prior archives. No repeat-until-green run or
benchmark workload reduction was used. PMU instructions/cycles/cache counters were NOT_RUN.

New native Graph normal baseline (100 hot, 30 cold samples): record+submit p50/p95/max
**25.5/42.4/57.9 microseconds**, zero first-party heap. Cold compile with prebuilt Shader/PSO candidates
**38.9/43.0/62.9 microseconds**, 4,470 allocations / 679,590 bytes total over 30 compilations.
C9, C10, C11 and C12/C13 phase samples and GPU timestamps are archived. GPU timestamp intervals include
CPU submission gaps and are not advertised as isolated shader time or achieved FPS.

## Protection, dependency and reuse facts

`closure/closure.json` and `asan-closure/closure.json` verify actual File API targets, compile commands,
Ninja includes, source tracking, codegen job/depfiles, public-header TUs and linked libraries:

- Core has no component dependency; Transport/Graph/Foundation depend on Core only at that layer.
- Foundation still has no Graph include/link dependency. `render_vulkan_graph` is the native execution
  layer; the separate cold Shader compiler links reflection. The hot linked consumer includes binding
  and record objects and excludes Program.cpp, reflection, relocation, ShaderAssets and SPIRV-Cross.
- No active Legacy, old installed SDK, Scene/ECS, Runtime, Feature implementation or Editor inputs.
- All **719 Legacy blobs/modes/sizes plus guard** match. Core/Transport/Graph, Description/Toolchain,
  FINAL and every historical report match the approved base objects. Foundation and F3 descriptor files
  changed only as documented conditional extensions; they are requalified, not claimed unchanged.
- `F3_CAPABILITY_STATUS.md` was append-only. No historical PARTIAL/NOT_RUN verdict was rewritten.
- User original HEAD/branch, NUL-delimited status, staged/unstaged binary diffs, untracked list and six
  original file byte hashes match preflight; `user-protection.json` and raw snapshots provide evidence.
- Eight changed public headers synchronized to three prefixes (**24 byte copies**), with receipts.
  This is not an installed-SDK or Android build qualification.

Historical V1 matrices and R0–F3 performance/qualification remain tied to their original SHAs.
Unchanged Core/Transport/F2 standalone historical performance campaigns were not repeated; their current
integration tests did run. F4 GPU/ASan evidence is new and is not borrowed from F3. `test-impact.json`
records exact changed blobs, real dependencies, executed sets and narrowly reused historical facts.

## Open items and STOP

1. **F4 performance gate OPEN:** determine the remaining fixed-binding caller/link/native-record cost
   with the unchanged workload and preserved controls. Do not rewrite F2 or waive the gate from a single
   favorable run. A new production correction requires a new I and applicable independent qualification.
2. Native shape/draw limits stated above remain explicit NOT_IMPLEMENTED; no device-fallback claim or
   unilateral reduction of FINAL scope. Review these before any assertion of complete native coverage.
3. Physical Device Lost was not induced; fault seam verifies the precise terminal contract after real GPU
   completion. No recovery Runtime is implemented. Hardware without tested capabilities, Linux, Android,
   installed-SDK consumers, swapchain/presentation, Runtime pacing and product tests are **NOT_RUN**.
4. F5 Runtime, F6 plugin/code-pin integration, F8 business/material integration and F11 product are
   **NOT_IMPLEMENTED/NOT_RUN** here. Historical transfer_idle and skinning WAR are not reclassified.

`NATIVE_RENDER_GRAPH = IMPLEMENTED_WITH_RECORDED_LIMITATIONS`
`F4 = PARTIAL`
`V2_PRODUCT = EXPECTED_UNAVAILABLE`
`F5 = NOT_AUTHORIZED`
`STOP`

## Complete implementation change list

68 files relative to the approved entry; V adds only this report. Machine-readable list with before/after
Git blobs is in `test-impact.json`.

```text
cmake/render-v2-bootstrap/verify_f4.py
docs/render-v2/F3_CAPABILITY_STATUS.md
docs/render-v2/F4_IMPLEMENTATION.md
docs/render-v2/F4_TEST_IMPACT.md
docs/render-v2/F4_WORK_ORDER.md
modules/function/render/vulkan/CMakeLists.txt
modules/function/render/vulkan/include/lux/engine/render/vulkan/descriptor/ImageBindings.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/device/Device.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/graph/Executable.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/memory/Memory.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/pipeline/Graphics.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/retirement/Retirement.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/shader/Bindings.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/transfer/Submission.hpp
modules/function/render/vulkan/pinclude/GraphNative.hpp
modules/function/render/vulkan/src/descriptor/ImageBindings.cpp
modules/function/render/vulkan/src/device/Device.cpp
modules/function/render/vulkan/src/graph/Compile.cpp
modules/function/render/vulkan/src/graph/Copy.cpp
modules/function/render/vulkan/src/graph/Diagnostics.cpp
modules/function/render/vulkan/src/graph/Export.cpp
modules/function/render/vulkan/src/graph/Frame.cpp
modules/function/render/vulkan/src/graph/Recipes.cpp
modules/function/render/vulkan/src/graph/Record.cpp
modules/function/render/vulkan/src/graph/Resources.cpp
modules/function/render/vulkan/src/graph/Sync.cpp
modules/function/render/vulkan/src/memory/Memory.cpp
modules/function/render/vulkan/src/pipeline/Graphics.cpp
modules/function/render/vulkan/src/shader/Bindings.cpp
modules/function/render/vulkan/src/shader/Program.cpp
modules/function/render/vulkan/src/shader/ProgramValue.cpp
modules/function/render/vulkan/src/transfer/Submission.cpp
modules/function/render/vulkan/test/CMakeLists.txt
modules/function/render/vulkan/test/Device.cpp
modules/function/render/vulkan/test/NativeGraph.cpp
modules/function/render/vulkan/test/NativeGraphAlias.cpp
modules/function/render/vulkan/test/NativeGraphBenchmark.cpp
modules/function/render/vulkan/test/NativeGraphCandidate.cpp
modules/function/render/vulkan/test/NativeGraphConditional.cpp
modules/function/render/vulkan/test/NativeGraphCopy.cpp
modules/function/render/vulkan/test/NativeGraphHzb.cpp
modules/function/render/vulkan/test/NativeGraphImport.cpp
modules/function/render/vulkan/test/NativeGraphRaster.cpp
modules/function/render/vulkan/test/NativeGraphReadWrite.cpp
modules/function/render/vulkan/test/NativeGraphShared.cpp
modules/function/render/vulkan/test/NativeGraphSupport.cpp
modules/function/render/vulkan/test/NativeGraphSupport.hpp
modules/function/render/vulkan/test/NativeGraphViews.cpp
modules/function/render/vulkan/test/Objects.cpp
modules/function/render/vulkan/test/ShaderHotLink.cpp
modules/function/render/vulkan/test/shader/CMakeLists.txt
modules/function/render/vulkan/test/shader/F4Accumulate.comp.lglsl
modules/function/render/vulkan/test/shader/F4Accumulate.hpp
modules/function/render/vulkan/test/shader/F4CopyBuffer.hpp
modules/function/render/vulkan/test/shader/F4CopyImage.hpp
modules/function/render/vulkan/test/shader/F4CopyUpload.hpp
modules/function/render/vulkan/test/shader/F4Fill.comp.lglsl
modules/function/render/vulkan/test/shader/F4Fill.hpp
modules/function/render/vulkan/test/shader/F4HostRead.hpp
modules/function/render/vulkan/test/shader/F4Hzb.comp.lglsl
modules/function/render/vulkan/test/shader/F4Hzb.hpp
modules/function/render/vulkan/test/shader/F4Local.frag.lglsl
modules/function/render/vulkan/test/shader/F4Local.hpp
modules/function/render/vulkan/test/shader/F4Raster.hpp
modules/function/render/vulkan/test/shader/F4Readback.hpp
modules/function/render/vulkan/test/shader/F4Views.frag.lglsl
modules/function/render/vulkan/test/shader/F4Views.hpp
modules/function/render/vulkan/test/shader/F4Views.vert.lglsl
```
