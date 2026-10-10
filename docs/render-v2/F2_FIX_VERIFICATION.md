# F2-FIX — Independent Logical Graph qualification

**F2-FIX = PASS (Logical Graph correctness).** Native RenderGraph remains **NOT_IMPLEMENTED**; F3 is **NOT_AUTHORIZED**. This is a locked-commit independent-clone qualification, not independent human review. Historical F2 PASS is unchanged.

## Identity and isolation

| Item | Value |
|---|---|
| Approved baseline | `3216bc8d19567abf8fc00c76bab7fdb3fff77e1a` |
| Implementation I | `946cf0866eda3566abc2fe2d06581ff868e157c3` |
| Previous production implementation | `f2318d708609e686ed49bdf61de1bade9b17279e` |
| Frozen Legacy | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| Independent clone | `D:/LuxQualification/render-v2-f2-fix-946cf0866eda/source` |
| Builds | Sibling `build` and `asan`, outside source |
| Evidence | `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F2-FIX/946cf0866eda3566abc2fe2d06581ff868e157c3/` |

Remote HEAD matched the approved baseline at preflight. I was committed before qualification; `ValidateTrackedSnapshot.cmake` passed. An independent `clone --no-hardlinks --no-checkout`, detached at I, was used for all formal builds/tests. The source remained clean, without production patches. V adds only this report; its containing Git commit is the verification SHA.

## P0 correctness: FIXED

Before production changes, the minimal retained `View A --ORDER--> Scene B` fixture compiled against the actual approved F2 library printed `accepted=1` and exited **1** on the required scope rejection assertion. `preflight/ScopeProof-before.cpp`, `repro-before.json`, `repro-before.log` and `before-binaries.json` preserve the source, real command, baseline identities and hashes. Both passes are explicitly live through diagnostic retention, so culling cannot conceal the defect. The same scenario in final production returns `kGraphScopeConflict`.

C7 now proves the live execution predecessor relation including ORDER/RAW/WAR/WAW and fallback paths. C5 still propagates only data-producer liveness. A dead ORDER predecessor is culled; a live narrow predecessor rejects Scene sharing with actual PassKey/name/edge/path diagnostics. Explicit Scope policy replaces ordinal comparisons. Scene → Scene chains include ordered predecessors' actual Params, imports, ready epochs and condition state; a changed prerequisite prevents sharing. Independent Scene work remains shareable across different Cameras/Views.

Tests cover real Export roots for View/Target ORDER → Scene, transitive View → Scene → Scene and View → View → Scene, WAR/WAW/RAW conflicts with the exact scope error, legal Scene → View/Target/Scene, Target → View rejection, input/backing/epoch/revision changes, conditions with live fallback/culling, and dead ORDER culling. Conditional prerequisite unions are conservative; no invocation branch is assumed safe merely because another branch is disabled. No resource-version redesign or native execution was introduced.

## P1 disposition

| Item | Status and bounded change |
|---|---|
| Separate relations | **IMPLEMENTED**: execution edges, data liveness and derived live share prerequisites have distinct purposes; one hazard record authority |
| Algorithm separation | **IMPLEMENTED**: cycle detection, scheduling, lifetimes, diagnostics, liveness and scope proof are private free algorithms; C3 selection remains in the sole analyzer |
| Role classification | **IMPLEMENTED**: unique constexpr functions in neutral PassContract; Graph Builder/Schema and Shader validator use them; every existing role and invalid enums tested |
| Fingerprint semantics | **IMPLEMENTED approved diagnostic-only option**: `fingerprint()` removed, `diagnosticDigest()` expressly cannot identify/cache structural plans; exact `matches()` remains authoritative |
| Cold scale / warm performance | **IMPLEMENTED**: four scale points and unchanged paired binding workload, raw samples retained |

Full Type/Algorithm Responsibility Inventory, owner/borrow/error/cold/hot details and rationale: [F2_FIX_INVENTORY.md](F2_FIX_INVENTORY.md). No new production owner, Manager, parser, thread, Runtime or execution framework exists. A structural/native cache hash is deliberately **not supplied**; later cache work must define complete identity rather than adopt diagnostic digest. Two equal-digest cases with changed Shader interface or Texture format correctly fail exact compatibility.

The only conditional files are `PassContract.hpp` (add shared constexpr classifiers; no enum/schema layout changes) and `PassValidation.cpp` (remove private ordinal helper and call the shared classifier at its two consumers). F3-PRE-02 is not addressed by this classifier cleanup. `c3-preservation.json` checks the C3 version/range implementation against baseline, allowing only the removed ordinal scope rejection (moved to C7) and the extracted cycle-function call.

## Independent tests and dependencies

Windows x64, MSVC 14.44.35207, C++20, RelWithDebInfo, Ninja. Both configurations used `all -j 4 -- -k 0`, twice; both second rounds reported **no work to do**. Builds and real-device tests were sequential.

| Gate | Result |
|---|---|
| Ordinary full CTest | **124/124 PASS**, 30.58 s |
| Full MSVC ASan CTest | **124/124 PASS**, 48.61 s |
| Earlier obligations | All **116** names retained, no test removed or downgraded |
| Added cases | `scope_orders`, `scope_hazards`, `scope_sharing`, `scope_conditions`, `scope_culling`, `scope_digest`, `scope_roles`, `scale_smoke` (all `render.graph.*`) |
| CPU oracles | Original 300 random graphs, 10k bindings and 700 mixed Buffer/Texture oracle pass |
| Generated author/Shader contract | **61/61 generated artifact hashes equal baseline**, in ordinary and ASan; 11 SPIR-V modules validated/reflected with real tools |
| Header/Concept/dependency negatives | Existing standalone public C++20 headers and real compile rejection tests pass |
| Actual closure | File API, compiler/Ninja inputs, link libraries and codegen jobs checked; 291 resolved compiler headers in the audited closure |

ASan used `/fsanitize=address /Zi /MD` and the ABI-compatible instrumented SPIRV-Cross dependency. Package and original source/configure/build receipts are copied to the evidence archive. No annotations were suppressed. Full verbose outputs, not only CTest summaries, are preserved in `test.log` / `asan-test.log`; Testing directories were copied before inventory queries could overwrite LastTest.log.

Production Graph remains Core-based with the existing lightweight neutral headers. It does not link or compile against Vulkan, Scene, Runtime, Transport, Editor or offline Shader Toolchain. The scale **test executable** alone uses Windows Psapi for process-memory measurement. Production codegen remains the existing Meta/emitter/glslc/reflection chain. See `closure/closure.json`, `asan-closure/closure.json` and `generated-preservation.json`.

Unchanged R4 GPU regression tests passed with **0 validation errors** on NVIDIA GeForce RTX 4070 Ti, driver raw 2480242688, supported API 1.4.325. The existing fixture enables API 1.3, synchronization2, Khronos validation and synchronization validation; no device extensions, instance debug-utils/validation-features extensions. Galaxy Overlay layer naming warnings remain in raw output (three per affected instance); they are not suppressed or called zero warnings. Performance fixtures retain their validation-disabled measurement mode. This is Foundation regression, not new Graph native GPU qualification.

## Warm binding and cold compilation

Unchanged `Benchmark.cpp`: seven alternating pairs, **1,000,000 operations each**, 10k warmup, batch size 1000, affinity mask 4, identical checksums. All samples are retained; statistics compare medians across all seven runs. **First-party C++ allocations and bytes remain 0 in every before/after run.** Binary hashes and production source SHAs are recorded.

| Statistic | Before | After | Change |
|---|---:|---:|---:|
| p50 | 10.8 ns/op | 11.1 ns/op | +2.78% |
| p95 | 10.8 ns/op | 11.2 ns/op | +3.70% |
| Median run maximum | 261.7 ns/op | 144.2 ns/op | -44.90% |

The p50/p95 gate passes. This is not a rendering-throughput or GPU improvement claim. Cold compilation is measured separately. The existing fixed 64-pass/16-resource benchmark remains source-identical: current p50 **599.6 us**, p95 **766.7 us**, max **2160.8 us**, 372000 allocation calls / 92165200 bytes over 200 operations. The historical F2 receipt (about 1.460/1.664 ms) is retained without rewriting it.

Mixed scale workload: 16 resources, half Image/half Buffer; mip count grows 1/2/4/8, two layers, Buffer subranges 2/4/8/16, explicit producer versions. Culling and diagnostics are enabled. One unmeasured warmup, 20 repetitions at 64/128 and five at 256/512; raw samples and per-run maxima are retained. Identical fixture source (normalizing Git checkout line endings only, recorded in `scale-source-identity.json`) is compiled against before/after production libraries; dependency/version/hazard counts match.

| Passes | Cells | Dependencies / versions / hazards | Before p50 / p95 (us) | After p50 / p95 / max (us) | Repeats | After allocation calls / bytes (total) | After peak process working set (bytes) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 32 | 32 / 96 / 128 | 1451.3 / 2059.7 | 612.4 / 825.3 / 825.3 | 20 | 36780 / 9256200 | 6176768 |
| 128 | 64 | 64 / 192 / 256 | 7785.6 / 8870.7 | 1231.6 / 2067.6 / 2067.6 | 20 | 66940 / 17417020 | 6668288 |
| 256 | 128 | 128 / 384 / 512 | 53924.1 / 56381.9 | 2894.7 / 4122.6 / 4122.6 | 5 | 31760 / 9070980 | 7483392 |
| 512 | 256 | 256 / 768 / 1024 | 410027.4 / 412565.8 | 7161.1 / 7941.9 / 7941.9 | 5 | 61510 / 18801065 | 9043968 |

Word-parallel closure reduces the constant factor of transitive closure (still O(P³ / word size)); worklist liveness replaces repeated cubic scans with O(P²) work. Version semantics are preserved. These measurements are bounded to the tested scale and fixture, not a promise of linear arbitrary-graph compilation. Peak process working set includes fixture/runtime/retained warm plan overhead; it is not exact live first-party heap. No completeness reduction, sample deletion or cold/warm metric mixing was used.

## Protection, changed files and evidence

- **719/719** frozen source blobs/modes/sizes and the Legacy guard unchanged.
- Core, all Transport including its previously approved Error.hpp alignment, R4 Vulkan, FINAL and every historical verification report retain exact Git identity.
- Other Shader/Description files are unchanged; only the two exact conditional paths differ.
- Original worktree HEAD/branch/status/staged and unstaged binary diffs/untracked list and all six modified-file byte hashes match preflight.
- Plan.hpp, Schema.hpp and neutral PassContract.hpp synchronized to three install include prefixes: **nine copies**, with backups/hashes. This is not Android or installed SDK qualification.
- Clean clone is unchanged at I; Root/product, Engine/Editor/Scene/Runtime/Feature and native Graph are untouched.

Implementation changes exactly these 13 files; V adds only this report:

```text
cmake/render-v2-bootstrap/verify_f2_fix.py
docs/render-v2/F2_FIX_INVENTORY.md
docs/render-v2/F2_FIX_WORK_ORDER.md
engine/toolchain/shader/src/PassValidation.cpp
modules/function/render/graph/README.md
modules/function/render/graph/include/lux/engine/render/graph/Plan.hpp
modules/function/render/graph/include/lux/engine/render/graph/Schema.hpp
modules/function/render/graph/src/Builder.cpp
modules/function/render/graph/src/Plan.cpp
modules/function/render/graph/test/CMakeLists.txt
modules/function/render/graph/test/ScaleBenchmark.cpp
modules/function/render/graph/test/ScopeProof.cpp
modules/resource/description/include/lux/engine/description/PassContract.hpp
```

Evidence `manifest.json` covers **954 files**, SHA-256 **`770fd437aeff43c1e0b41626264dbd9a466b5d47f5143950ebbd2b9963fb11f1`**. Manifest and its separate hash file exclude themselves. It includes source/command identities, before-failure reproducer, builds, verbose tests, sanitization dependency, real closure/codegen/SPIR-V artifacts, generated equality, performance samples, protection and install-sync receipts. Procedures are preserved in the archive.

## Final boundary

```text
F2-FIX = PASS (Logical Graph correctness)
NATIVE_RENDER_GRAPH = NOT_IMPLEMENTED
F3_PRE_01 = OPEN
F3_PRE_02 = OPEN
V2_PRODUCT = EXPECTED_UNAVAILABLE
F3 = NOT_AUTHORIZED
NEXT = Await user review
STOP
```

New native Graph execution/images, Runtime/FrameLoop, product, full installed SDK, Linux and Android: **NOT_RUN**. No past PASS/PARTIAL/NOT_RUN fact is reclassified. F3 requires separate user authorization and resolution of its still-open integer Clear and Shader include/declaration prerequisites.
