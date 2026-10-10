# F3 independent qualification — PARTIAL / STOP

## Locked identities

| Item | Identity |
|---|---|
| Approved F2-FIX base | `c986e69765c9dd222469d4cf5090aa72549966dc` |
| Frozen V1 | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| F3 work-order amendment | `28b42ea3255590f149f497ab1c6653abf0fec2ae` |
| Main production implementation | `7ca037aa113b712c19312bdfbc8919848ebcfb42` |
| Intermediate candidate | `15c82325a42d4b12e919478a1268f7be2e786a82` |
| **Final implementation I** | **`0fbbbd6db3aa4ad53283522e4a54d96556b5b478`** |
| Verification V | The commit adding only this report; resolve with `git log -1 --format=%H -- docs/render-v2/F3_VERIFICATION.md` |
| Branch | `codex/render-v2`; V is a direct child of final I |

Final I was validated with ValidateTrackedSnapshot, independently cloned using `--no-hardlinks --no-checkout`,
and checked out detached. Source: `D:/LuxQualification/render-v2-f3-0fbbbd6db3aa/source`.
Normal and ASan builds are sibling `build/` and `asan/`, outside source. No qualification-source patch.
The intermediate candidate passed normal tests but was superseded: native InputAttachment capability
is now rejected because dynamic-rendering local read is not enabled. All final qualification was rerun
on final I, including that negative case. Initial measurements and intermediate attempts remain archived.

## Decision and blocking result

**F3 = PARTIAL. Performance qualification = FAIL (unexplained regression). STOP; F4 is not authorized.**
Functional, GPU, sanitizer and dependency checks passed, but they do not override the performance gate.
Final paired cold compilation at 256 passes regressed **p50 11.4479%, p95 12.2421%**. Source/options and
allocation comparisons have not established a cause. No claim that this is harmless noise is made.
No samples were discarded, no algorithms weakened, and no F2 scheduler was rewritten to improve a score.

| Internal gate | Result | Evidence |
|---|---|---|
| F3.0 typed integer/float Clear | PASS | Generated capture/invocation negatives and actual G05 attachment CLEAR/readback |
| F3.0 include/declaration ordering | PASS | Production emitter, glslc, spirv-val, reflection; 5 positive/9 negative cases |
| F3.1 LayoutPlan/full shapes | PASS | Complete owners, stage/array/PC/dynamic limits, shared layout GPU output |
| F3.2 relocation/re-reflection | PASS | Original/final validation, pair mapping, malformed/interface rejection |
| F3.3 native owners/candidate | PASS | Real graphics/compute creation, descriptor writes, rollback and last-good |
| F3.4 G01–G07 GPU / G08–G09 cold negatives | PASS | Detailed results below, validation errors 0 |
| F3.4 performance | PARTIAL / gate FAIL | Stable bindings pass; 256-pass cold compile exceeds 5% |
| Overall | **PARTIAL** | Do not treat component PASS as phase release |

F3-PRE-01 and F3-PRE-02 are closed by this report's author/native evidence. I09/I10/I11/I12 have F3
functional mechanism evidence. I42 remains **PARTIAL** until F8 Material integration. W01/W15 qualify
these native fixtures, not F9 business Features, F4 Native Graph or F5 Scene/View ownership. The cumulative
[F3_CAPABILITY_STATUS.md](F3_CAPABILITY_STATUS.md) resolves to this report and cannot imply overall PASS.

## Build, regression and sanitizer results

MSVC 19.44.35228, C++20, Ninja, RelWithDebInfo; Vulkan SDK 1.4.304.0; installed VMA/lux-cxx dependencies.
Each configuration built full `all -j 4 -- -k 0` twice; both second builds printed `ninja: no work to do`.

| Configuration | CTest | Time | Result |
|---|---:|---:|---|
| Normal | 137/137 | 32.54 s | PASS |
| Full MSVC AddressSanitizer | 137/137 | 62.01 s | PASS |

All original **124** names and obligations remain; 13 additions are listed below. The original 300 random
and 700 mixed Graph oracles, C7 proof, range/version/lifetime/plan-reuse and public-header/Concept negatives
remain. Old author tests only migrate the Clear value syntax. Graph Plan/Builder/codegen sources are
byte-identical to the approved base. Binding invocation validation was separated into a private ordinary
function to keep expanded Clear checks out of the no-invocation path; no validation was removed.

ASan flags apply to all first-party targets; the existing matching annotated SPIRV-Cross ASan dependency
was reused with its source/configure/build receipts. No STL annotation suppression. SPIRV-Tools uses its
installed shared **C ABI**, with no third-party source/ABI modification. OS, driver and that external DLL
are not claimed to be newly instrumented by this qualification.

New CTest names:

- `render.graph.f3_include_contract`
- `render.graph.typed_color_clear`
- `render.vulkan.attachment_clear`
- `render.vulkan.compute_bindings`
- `render.vulkan.postprocess_w01`
- `render.vulkan.raster_state`
- `render.vulkan.shader_assets`
- `render.vulkan.shader_benchmark`
- `render.vulkan.shader_candidate`
- `render.vulkan.shader_cold`
- `render.vulkan.shader_hot_link`
- `render.vulkan.shader_layout`
- `render.vulkan.shared_owners`

## Native GPU evidence

Physical device **NVIDIA GeForce RTX 4070 Ti**, vendor 4318/device 10114, driver **591.86.0.0**
(raw 2480242688), physical API **1.4.325** (4211013), queue family 0. Native target is Vulkan 1.3;
Device explicitly enables synchronization2 and requested dynamic rendering. Khronos validation 1.4.304
and synchronization validation are enabled in correctness fixtures. Descriptor indexing/local read,
shaderInt64 and other optional features are not enabled merely because hardware reports support.

Queried limits include 32 bound sets, 256-byte PC, 64-byte UBO alignment and 16-byte SSBO alignment.
Full device properties, features, extensions, formats and layers are in `vulkan-device.log`. Optional
indexing capabilities are queried but execution is NOT_RUN; unsupported requests are cold errors.

| Workload | Actual result |
|---|---|
| G01 / W01 | Tonemap HDR max error 5.96046e-08; sRGB 0.00186992; 3x3 Blur 5.96046e-08; two-input Composite and additive variant 1.19209e-07. Float tolerance 2e-05, sRGB 1.1/255. Individual/chained CPU oracle. |
| G02/G04 | Exact SSBO/storage image output; UBO offset 64, SSBO offset 16, two descriptor sets, array count 2, stride 16 and PC count 4. Range/owner/type/array/alignment negatives retained. |
| G03 / W15 | Two graphics pipelines consume complementary VS/FS subsets while retaining all four full shared-owner bindings; exact pixels and complete compatible shape. |
| G05 | Real `loadOp=CLEAR`: UINT 16777217/0xffffffff, RGBA UINT, SINT -1/min/max, float HDR and sRGB; exact byte readback. No direct clear-image substitution. |
| G06 | Samples 1 and 4, depth/stencil rejection, blend and resolve; exact expected (0.75,0.5,0,0.5). Test-local recording only. |
| G07 | 10 native fault rollbacks, unchanged last-good pixels; real old/new submissions 2/3, completed serial 3; old PSO destroyed only after fence proof. Moved/destroyed native owners counted with private test seam. |
| G08 | 16 cold budget/flag negatives plus dynamic-range negatives. Reduced limit snapshots explicitly synthetic; actual device limits separately recorded. |
| G09 | Malformed/version/length/duplicate/missing/alias/grouped/multiple-entry/unknown-variable/schema and VS/FS location-7 mismatch rejected before native creation. |

**Validation errors: 0.** Three GalaxyOverlay layer-name warnings per native instance are preserved,
not filtered. vulkaninfo additionally reports the OBS layer's older API warning. G08/G09 are cold
negative tests, not fake readback groups. Private G07 hooks only delay CPU fence observation and inject
native failure; actual submissions/waits remain real. Production and benchmark libraries lack test hooks.

Each configuration archives and independently validates/reflects **44 actual SPIR-V modules**, including
relocated modules. Original/generated GLSL, schemas, original/final SPV, mapping files, numerical readbacks,
commands/exits and hashes are retained. This is not F4 alias/queue/barrier/record qualification.

## Performance: passing bindings, blocked cold compile

Final comparison uses seven alternating one-million-operation Graph binding pairs, 10k warmup, affinity
mask 4 and equal ABOVE_NORMAL priority. Checksums/work match. Both before/after allocate **0 times/0 bytes**.

| Graph binding | F2-FIX ns/op | F3 ns/op | Change |
|---|---:|---:|---:|
| Median p50 | 10.8 | 10.6 | -1.85% |
| Median p95 | 10.8 | 10.7 | -0.93% |
| Median run max | 111.4 | 224.6 | +101.62% (retained; gate is p50/p95) |

The old scale binary has only 5/20 samples per process, so each per-process p95 equals its maximum.
The final protocol, fixed in I, pools all samples from **35 alternating pairs per size**, retaining every
raw/per-run result. No failing preliminary dataset is replaced or omitted.

| Passes | p50 before/after us | p95 before/after us | p50 / p95 change |
|---:|---:|---:|---:|
| 64 | 613.1 / 613.6 | 791.0 / 734.4 | +0.08% / -7.16% |
| 128 | 1226.2 / 1238.4 | 1526.4 / 1576.2 | +0.99% / +3.26% |
| **256** | **2814.5 / 3136.7** | **3029.7 / 3400.6** | **+11.45% / +12.24% — FAIL** |
| 512 | 7225.1 / 7182.1 | 8307.7 / 7932.4 | -0.60% / -4.52% |

`performance-investigation.json` proves unchanged Plan.cpp/Plan.hpp/benchmark bytes, equal MSVC options,
identical workload counts and allocation counts/bytes. These facts **do not explain the timing regression**.
The initial binding regression was corrected by separating invocation validation; all initial measurements,
A/A exploration and revised controlled sampling remain in `development-performance/`.

New F3 workloads have no equivalent V1 baseline. Native binding recipe: **1M calls, p50 12.2 ns,
p95 12.6 ns, max 52.8 ns, 0 C++ allocations**. Separate cold baseline (100 samples, five warmups):

| Cold operation | p50 / p95 / max us | C++ allocations / bytes, all 100 samples |
|---|---:|---:|
| Complete layout | 0.7 / 0.8 / 3.0 | 1000 / 95200 |
| Vertex relocation+validation | 467.9 / 612.9 / 786.5 | 188000 / 21585600 |
| Complete candidate including cook/file IO | 2338.3 / 2697.9 / 3422.5 | 990300 / 115755800 |
| Native graphics PSO create/destruct | 1.8 / 1.9 / 74.2 | 100 / 3200 |
| Descriptor pool/sets/write | 0.9 / 1.0 / 3.1 | 200 / 1600 |

These repeatedly use the same device/state and a warmed driver; they do not claim first-ever driver
compilation cost. GPU shared 2x2 draw+barriers+readback timestamp p50/p95/max: **4.224/4.416/4.448 us**.
Allocation census is explicitly disabled for the GPU timestamp fixture. Driver malloc, VRAM peak and
isolated shader throughput were NOT_MEASURED. Raw sample arrays and max values are in native-performance.json.

## Ownership, source and dependency proof

See [F3_LAYOUT_CONTRACT.md](F3_LAYOUT_CONTRACT.md), [F3_SHADER_CONTRACT.md](F3_SHADER_CONTRACT.md),
[F3_GPU_ORACLES.md](F3_GPU_ORACLES.md) and [type/provenance inventory](F3_IMPLEMENTATION_INVENTORY.md).
NativeShaderProgram owns existing R4 modules/layouts/PSO in safe destruction order; Device is borrowed.
BoundDescriptorSets owns its immutable pool/sets and borrows backing/program through last GPU use.
G07's test owner explicitly retains the composite until R4 completion; no production reload service or
Graph-aware extension of Foundation RetirementQueue is claimed. Catalog replacement returns old cooked
ownership; it cannot stand in for a GPU pin. Buffer/Image/transfer/retirement stay native mechanisms.

Both configurations independently checked **319 actual compiler headers** and **108 generated artifact
hashes**, CMake File API, source/include/link, Ninja deps and real codegen job/depfile inputs. Public C++20
heads compile independently. Core/Transport/Graph remain free of native Vulkan/toolchain dependencies.
`render_vulkan` Foundation still depends downward on Core/Vulkan/VMA. Graph-aware cold code is in the
same module's `render_vulkan_shader_compiler`; actual binding-consumer link map/imports contain no
Program/reflection/relocation/catalog object or SPIR-V DLL. No source is supplied by Legacy/old SDK.

- 719 Legacy blobs/modes/sizes plus guard unchanged.
- Core, Transport, FINAL and every pre-F3 historical document/report unchanged.
- F2 Graph Plan/Builder/codegen unchanged; original R4 transfer/retirement source and headers unchanged.
  R4 Device/Memory/Descriptor/Pipeline extensions are authorized, listed changes rather than falsely
  described as byte-unchanged. Their original GPU/ownership/fault tests passed again.
- Original user checkout HEAD/branch/status/staged+unstaged binary diffs/untracked list and all six byte
  hashes are identical to preflight. No stash/reset or restoration over user changes.
- 18 public headers copied into three installation prefixes (54 copies). Installed bytes match locked
  Git blobs; clean Windows checkout CRLF normalization is checked separately and recorded. This is
  header synchronization only; Android and complete installed SDK qualification remain NOT_RUN.

## Evidence and outstanding admission

Archive:
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F3/0fbbbd6db3aa4ad53283522e4a54d96556b5b478/`

2,764 files in the SHA-256 manifest. `manifest.json` SHA-256:
`9e49fec0f812236494ccac401cfc97ace8db7b7d26295d90871aa7b1953e1992`.

Key receipts: setup.json, configure/build-1/build-2/test and asan-* command/log pairs, both closure/
reports, spirv-commands.json, native/graph generated artifacts, readback evidence, native-performance.json,
performance/performance.json, performance-investigation.json, protection.json, header-sync.json,
implementation.patch/log, changed-files.json and reused ASan dependency receipts.

Before F3 can be released: explain or correct the 256-pass regression without weakening workload or
changing frozen F2 semantics; qualify a new implementation baseline if production changes are needed;
retain this PARTIAL report and its datasets. A later report must not retroactively rewrite this result.
Only an accepted F3 qualification plus explicit user authorization permits F4.

NOT_RUN/NOT_IMPLEMENTED: Linux/Android qualification, full installed SDK/plugin ABI, Material hot reload,
Native Graph compiler/alias/cross-queue scheduler, Runtime/FrameLoop/Scene/View integration, full Legacy
Features and final product matrix. Optional native local read/indexing execution not enabled in F3.

```text
F3 = PARTIAL
PERFORMANCE_GATE = FAIL / UNEXPLAINED_REGRESSION
NATIVE_RENDER_GRAPH = NOT_IMPLEMENTED
RENDER_RUNTIME = NOT_IMPLEMENTED
V2_PRODUCT = EXPECTED_UNAVAILABLE
F4 = NOT_AUTHORIZED
STOP
```

## Complete implementation file list (approved base → final I)

The verification commit adds only this report; the following 89 files belong to implementation commits:

```text
cmake/render-v2-bootstrap/measure_f3.py
cmake/render-v2-bootstrap/verify_f3.py
docs/render-v2/F3_CAPABILITY_STATUS.md
docs/render-v2/F3_GPU_ORACLES.md
docs/render-v2/F3_IMPLEMENTATION_INVENTORY.md
docs/render-v2/F3_LAYOUT_CONTRACT.md
docs/render-v2/F3_SHADER_CONTRACT.md
docs/render-v2/F3_WORK_ORDER.md
engine/toolchain/shader/CMakeLists.txt
engine/toolchain/shader/include/lux/engine/toolchain/shader/PassValidation.hpp
engine/toolchain/shader/include/lux/engine/toolchain/shader/ShaderAssets.hpp
engine/toolchain/shader/include/lux/engine/toolchain/shader/SpirvRelocation.hpp
engine/toolchain/shader/src/PassValidation.cpp
engine/toolchain/shader/src/ShaderAssets.cpp
engine/toolchain/shader/src/SpirvRelocation.cpp
engine/toolchain/shader/src/lglsl/LglslEmitter.cpp
modules/function/render/graph/include/lux/engine/render/graph/Authoring.hpp
modules/function/render/graph/include/lux/engine/render/graph/Bindings.hpp
modules/function/render/graph/include/lux/engine/render/graph/Definition.hpp
modules/function/render/graph/include/lux/engine/render/graph/Schema.hpp
modules/function/render/graph/src/Bindings.cpp
modules/function/render/graph/src/Definition.cpp
modules/function/render/graph/test/AuthoringLogical.cpp
modules/function/render/graph/test/CMakeLists.txt
modules/function/render/graph/test/ColorClear.cpp
modules/function/render/graph/test/authoring/Builder.cpp
modules/function/render/graph/test/authoring/F3Complex.comp.lglsl
modules/function/render/graph/test/authoring/F3ComplexHelper.lglslh
modules/function/render/graph/test/authoring/F3ComplexInner.lglslh
modules/function/render/graph/test/authoring/F3Includes.py
modules/function/render/graph/test/authoring/F3Tonemap.frag.lglsl
modules/function/render/graph/test/authoring/F3TonemapHelper.lglslh
modules/function/render/graph/test/authoring/F3Types.lglslh
modules/function/render/vulkan/CMakeLists.txt
modules/function/render/vulkan/include/lux/engine/render/vulkan/descriptor/Descriptors.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/descriptor/ImageBindings.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/device/Device.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/memory/Memory.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/pipeline/Graphics.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/pipeline/Pipeline.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/shader/Bindings.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/shader/Format.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/shader/Layout.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/shader/Program.hpp
modules/function/render/vulkan/src/descriptor/Descriptors.cpp
modules/function/render/vulkan/src/descriptor/ImageBindings.cpp
modules/function/render/vulkan/src/device/Device.cpp
modules/function/render/vulkan/src/memory/Memory.cpp
modules/function/render/vulkan/src/pipeline/Graphics.cpp
modules/function/render/vulkan/src/pipeline/Objects.cpp
modules/function/render/vulkan/src/pipeline/Pipeline.cpp
modules/function/render/vulkan/src/shader/Bindings.cpp
modules/function/render/vulkan/src/shader/Layout.cpp
modules/function/render/vulkan/src/shader/Program.cpp
modules/function/render/vulkan/test/AttachmentClear.cpp
modules/function/render/vulkan/test/CMakeLists.txt
modules/function/render/vulkan/test/ComputeBindings.cpp
modules/function/render/vulkan/test/Postprocess.cpp
modules/function/render/vulkan/test/RasterState.cpp
modules/function/render/vulkan/test/ShaderAssets.cpp
modules/function/render/vulkan/test/ShaderBenchmark.cpp
modules/function/render/vulkan/test/ShaderCandidate.cpp
modules/function/render/vulkan/test/ShaderColdFailures.cpp
modules/function/render/vulkan/test/ShaderHotLink.cpp
modules/function/render/vulkan/test/ShaderLayout.cpp
modules/function/render/vulkan/test/ShaderTestSupport.hpp
modules/function/render/vulkan/test/SharedFixture.hpp
modules/function/render/vulkan/test/SharedOwners.cpp
modules/function/render/vulkan/test/shader/CMakeLists.txt
modules/function/render/vulkan/test/shader/F3Blur.frag.lglsl
modules/function/render/vulkan/test/shader/F3Blur.hpp
modules/function/render/vulkan/test/shader/F3Composite.frag.lglsl
modules/function/render/vulkan/test/shader/F3Composite.hpp
modules/function/render/vulkan/test/shader/F3Raster.frag.lglsl
modules/function/render/vulkan/test/shader/F3Raster.hpp
modules/function/render/vulkan/test/shader/F3Raster.vert.lglsl
modules/function/render/vulkan/test/shader/F3SharedA.frag.lglsl
modules/function/render/vulkan/test/shader/F3SharedA.hpp
modules/function/render/vulkan/test/shader/F3SharedA.vert.lglsl
modules/function/render/vulkan/test/shader/F3SharedB.frag.lglsl
modules/function/render/vulkan/test/shader/F3SharedB.hpp
modules/function/render/vulkan/test/shader/F3SharedB.vert.lglsl
modules/function/render/vulkan/test/shader/F3SharedValue.hpp
modules/function/render/vulkan/test/shader/F3Storage.comp.lglsl
modules/function/render/vulkan/test/shader/F3Storage.hpp
modules/function/render/vulkan/test/shader/F3Tonemap.frag.lglsl
modules/function/render/vulkan/test/shader/F3Tonemap.hpp
modules/function/render/vulkan/test/shader/Fullscreen.vert.lglsl
modules/resource/description/include/lux/engine/description/Image.hpp
```
