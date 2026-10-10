# F2 — Complete Logical Graph independent qualification

Status: **PASS — F2 LogicalGraph**. Native RenderGraph is **NOT_IMPLEMENTED**. This report records qualification of a locked implementation in an independent clean clone, not an independent human review or permission to start F3.

## Identity and isolation

| Item | Value |
|---|---|
| Approved base | `c93f9b0ff35f556d6a95a00c585b51c21ca14b2d` |
| Initial implementation candidate I1 | `1c9288151c39907c217a96c0b42949abfbd26793` |
| Final qualified implementation I | `f2318d708609e686ed49bdf61de1bade9b17279e` |
| Frozen V1 reference | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| Independent source | `D:/LuxQualification/render-v2-f2-f2318d708609/source` |
| Regular / ASan builds | Sibling directories `build` / `asan`, outside source |
| Evidence archive | `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F2/f2318d708609e686ed49bdf61de1bade9b17279e/` |

The clone was created with `--no-hardlinks --no-checkout` and detached at I. `ValidateTrackedSnapshot.cmake` passed against the implementation and independent source. No production patch was applied inside qualification. The verification commit adds only this report; its Git commit identity is the verification SHA.

Supplemental review of I1 found that a `T&` template could deduce const T and accept a const vector temporary. A separate implementation correction added explicit deleted rvalue overloads and a compile-negative case. `borrow-defect-reproduction.json` records the same standalone reproducer compiling against I1 (exit 0) and failing with the deleted overload against final I (exit 2). The entire ordinary and ASan qualification was repeated on final I; I1 results are not substituted for it.

## Delivered contract and C0–C8 evidence

The production chain is typed generated Schema / Builder → owning Definition → free `compileLogicalGraph()` → owning readonly `LogicalGraphPlan`. The old static compile entry and whole-resource scheduler were migrated, not retained as a second implementation. Public API, type responsibility inventory, provenance, complexity and test migration are documented in [F2_CONTRACTS.md](F2_CONTRACTS.md).

| Contract | Implemented and tested behavior |
|---|---|
| C0 canonicalization | Closed Texture/Buffer descriptor and range values, derived kind, checked format/aspect/range, canonical full extents and scoped identity |
| C1 resolution | Required unique semantic producer, optional typed fallback, initialized import regions and typed output effects; missing/ambiguous sources fail |
| C2 interfaces | Existing generated Schema remains authoritative; captured fields become closed Shader/Sampler/Attachment/Transfer binding values; graphics/compute/transfer authoring and wrong-role negatives |
| C3 hazards/versions | Boundary-partitioned Buffer bytes and Texture aspect/mip/layer; explicit writer order/producer selection; per-cell RAW/WAR/WAW; disjoint same-pass uses; initialization/discard and explicit local-read legality |
| C4 scheduling | Stable canonical-name tie-break, deterministic order and concrete cycle path; no insertion-order choice of multi-writer version |
| C5 culling | Real Export/Present/Readback/ExternalWrite roots retain data producers; dead work is culled; order/anti edges alone do not create liveness |
| C6 lifetimes | Per-version/subresource intervals, exported terminal lifetime and compatible transient reuse candidates; no claim of GPU synchronization or physical alias safety |
| C7 scope | Scene/View/Target violations rejected; Scene sharing checks actual invocation inputs, revision and import epochs/backings, including transitive producers |
| C8 result/cache | One immutable owning result, deterministic JSON, full exact structural compatibility and field-level mismatch explanations; hash is only a lookup hint |

Camera, scalar bytes, sampler and Clear values have actual caller-owned `GraphInvocationData`; frame serial/time and import backing/offset/epoch are invocation facts. Their changes reuse the compatible logical plan while updating consumed values. Definition value equality still compares real defaults. Structural resource/access/Shader changes mismatch. Conditions select the proved alternative through `resourceFor()`; they cannot leave a consumer reading uninitialized storage.

`FrameGraphBindings` is a synchronous borrow of named live Plan, import storage and optional owning Invocation. Rvalue Plan, Invocation, ordinary vector and const vector are compile-rejected. The caller must keep named storage alive and unmoved; an already-dangling span is not made safe by this API. Native backing ownership, pins, waits and execution belong to later phases.

## Builds, tests and sanitization

Windows x64 MSVC 14.44.35207, C++20, Ninja, RelWithDebInfo; all build commands used `--target all -j 4 -- -k 0`. Regular and full ASan configurations both completed two build rounds; the second round reported `ninja: no work to do`.

| Check | Result | Raw evidence |
|---|---|---|
| Ordinary CTest | **116/116 PASS**, 33.44 s | `test.log`, `test.json` |
| Full ASan CTest | **116/116 PASS**, 53.27 s | `asan-test.log`, `asan-test.json` |
| Ordinary verbose repeat | **116/116 PASS**, 33.92 s | `test-verbose.log`, `test-verbose.json` |
| Full ASan verbose repeat | **116/116 PASS**, 50.75 s | `asan-test-verbose.log`, `asan-test-verbose.json` |
| Existing obligations | **99 retained**, none removed; 17 added | `baseline-tests.json`, `test-obligations.json` |
| Old Graph oracle | 300 random graphs, original reachability/lifetime checks and 10k binding reuse retained | `render.graph.*`, migrated `test/Graph.cpp` |
| New independent oracle | **700 mixed Buffer/Texture graphs PASS**; explicit byte/mip/layer/aspect version enumeration, independent of production cell partitioning | `render.graph.logical_oracle` |
| Original generated Shader contract | Production Meta, emitter, glslc, Schema reflection and rejection tests pass | 61 hashed generated artifacts; 11 SPIR-V modules independently validated/reflected/disassembled |
| Public C++20 headers / Concepts / forbidden dependency probes | PASS, including all five new compile negatives | Build logs, negative logs, File API and compiler dependency evidence |

The verbose repeats were necessary because CTest's later `--show-only=json-v1` inventory query replaced its temporary LastTest log. No source changed; full passed-test stdout and native validation messages are retained in the explicit verbose logs and copied Testing directories.

Full ASan uses `/fsanitize=address /Zi /MD` and the previously built ABI-compatible SPIRV-Cross package at `D:/LuxQualification/render-v2-f1-fix-spirv-asan-install`. Its package, source identity and original build receipts are copied into this archive. No container annotation suppression was added. All applicable Graph CPU tests and the full bootstrap suite ran, not only the earlier nine-test subset.

New tests are:

```text
render.graph.logical_authoring
render.graph.logical_compile_benchmark
render.graph.logical_conditions
render.graph.logical_culling
render.graph.logical_depth_stencil
render.graph.logical_feedback
render.graph.logical_initialization
render.graph.logical_oracle
render.graph.logical_providers
render.graph.logical_ranges
render.graph.logical_reject_TEMP_CONST_VECTOR
render.graph.logical_reject_TEMP_INVOCATION
render.graph.logical_reject_TEMP_VECTOR
render.graph.logical_reject_WRONG_SHADER
render.graph.logical_reject_WRONG_STORAGE
render.graph.logical_scopes
render.graph.logical_versions
```

The original ordered-stream random fixture now declares its writer order and selected producers explicitly; production still derives RAW/WAR edges. Original HZB coverage is upgraded to real subresource compilation, with OOB/feedback negatives retained. Depth/Stencil field coverage checks the exact union of the split aspect uses. These migrations are listed individually in F2_CONTRACTS rather than treating retained test names alone as proof.

## Native regression boundary

Unchanged R4 native tests ran as regression gates in both configurations. Device: NVIDIA GeForce RTX 4070 Ti, vendor 4318, device 10114, driver raw `2480242688`, supported API 1.4.325, queue family 0. Enabled API 1.3, synchronization2, no device extensions; instance extensions `VK_EXT_debug_utils` and `VK_EXT_validation_features`; `VK_LAYER_KHRONOS_validation` with synchronization validation.

Device, memory/pipeline, compute/buffer/image readback and fault tests report **0 validation errors**. Three Galaxy Overlay layer-name policy warnings per affected instance are preserved, not suppressed or described as zero warnings. The performance fixture disables validation as its original measurement contract specifies. These results do not qualify new native Graph execution or Graph-generated GPU images.

## Performance and allocation

The comparable binding workload uses the prior F1-FIX-2 implementation executable and final I executable: seven alternating before/after pairs, CPU affinity mask 4, 10k warmup, 1,000,000 operations per run, batch size 1000. Operation counts and checksums match. All seven pairs are retained; reported values are medians of each run's statistic, with no sample discarded. Executable hashes and before-source identity are archived.

| Metric | Before | After | Change |
|---|---:|---:|---:|
| p50 | 10.8 ns/op | 11.0 ns/op | +1.85% |
| p95 | 11.7 ns/op | 11.2 ns/op | -4.27% |
| max (median of run maxima) | 151.2 ns/op | 75.5 ns/op | -50.07% |
| First-party C++ allocations / bytes | 0 / 0 | 0 / 0 | No regression |

The p50/p95 gate passes; these measurements are not a claim of general rendering performance improvement. Raw data: `performance.json`, `performance-*-before.log`, `performance-*-after.log`.

Complete logical cold compile gets a new baseline because the former whole-resource algorithm did not do equivalent work: 64 passes, 16 resources, 200 compilations; p50 **1459.8 us**, p95 **1663.8 us**, max **6559.2 us**; 370,000 allocation calls / 91,869,200 bytes over the entire 200-operation sample. Cold allocation is allowed. Current reachability analysis is O(P^3), and subresource cell count affects cost; no large-graph scalability or native record performance claim is made. See `performance-cold.log`.

## Dependency closure and protection

Both ordinary `closure.json` and `asan-closure/closure.json` pass. Evidence includes real CMake File API, compile commands, Ninja compiler dependencies, link lines, codegen jobs and input files, and hashes of 61 generated artifacts. The production compiler header closure contains 289 unique resolved headers across the audited modules. Graph test/consumer links show Graph/Core and platform runtime libraries, not Vulkan, Transport, Scene, Editor or Shader Toolchain. Neutral Description headers remain the lightweight shared schema source; its Math/Script aggregate is not linked. Offline tools are confined to generation/validation.

- Frozen Legacy: **719/719** original blobs, modes and sizes match the frozen manifest. Its guard is not counted as a source file. The historical Legacy Shader fixture also matches its recorded original blob.
- Core, R4 Vulkan, Shader Toolchain, Description, FINAL, all historical reports and other protected paths retain their exact Git identities.
- Transport has exactly one user-authorized exception: alignment whitespace in `transport/Error.hpp`. All other Transport blobs are unchanged, and this file has identical content after whitespace removal. It is not reported as an unchanged Transport tree.
- Original user worktree HEAD, branch, staged/unstaged binary diffs, status, untracked list and all six modified-file byte hashes match preflight. The user's Transport edit was preserved and included as explicitly requested.
- Five changed Graph public headers plus the authorized Transport header were copied to Debug, RelWithDebInfo and Android include prefixes: **18 matching copies**. Backups and hashes are in `install-sync.json`. This is header synchronization, not installed SDK or Android qualification.
- Clean qualification source remains clean at I. Root/product CMake, Scene, Editor and Runtime were not modified. `git diff --check` passes.

## Complete implementation file list

The cumulative base → I change is these 25 files. Verification adds only this report.

```text
cmake/render-v2-bootstrap/verify_f2.py
docs/render-v2/F2_CONTRACTS.md
docs/render-v2/F2_WORK_ORDER.md
modules/function/render/graph/README.md
modules/function/render/graph/include/lux/engine/render/graph/Bindings.hpp
modules/function/render/graph/include/lux/engine/render/graph/Builder.hpp
modules/function/render/graph/include/lux/engine/render/graph/Definition.hpp
modules/function/render/graph/include/lux/engine/render/graph/Plan.hpp
modules/function/render/graph/include/lux/engine/render/graph/Schema.hpp
modules/function/render/graph/pinclude/lux/engine/render/graph/DefinitionAccess.hpp
modules/function/render/graph/src/Bindings.cpp
modules/function/render/graph/src/Builder.cpp
modules/function/render/graph/src/Definition.cpp
modules/function/render/graph/src/Plan.cpp
modules/function/render/graph/test/AuthoringLogical.cpp
modules/function/render/graph/test/Benchmark.cpp
modules/function/render/graph/test/CMakeLists.txt
modules/function/render/graph/test/Graph.cpp
modules/function/render/graph/test/Logical.cpp
modules/function/render/graph/test/LogicalBenchmark.cpp
modules/function/render/graph/test/Probe.cpp
modules/function/render/graph/test/authoring/Builder.cpp
modules/function/render/graph/test/authoring/Contracts.cpp
modules/function/render/graph/test/authoring/LocalIdentity.cpp
modules/function/render/transport/include/lux/engine/render/transport/Error.hpp
```

## Evidence manifest and remaining gates

The archive manifest covers **953 files**. SHA-256 of `manifest.json`: `7b1d05fd953e18e774df9b9b4d45bcf757d2b379229e57f431be6633bcd123bf`. The manifest and its separate hash file are excluded from their own entries.

`run.py`, `qualify-all.py`, `verify_f2.py`, `performance.py`, `extra-proof.py` and `archive.py` preserve commands, exit codes and verification procedure. The archive also includes implementation diff/log, ordinary/ASan build and test records, dependency/codegen data, SPIR-V inspection, negative compiler diagnostics, performance samples, protection receipts and installation synchronization hashes.

| Item | Status |
|---|---|
| F2 LogicalGraph | **PASS** |
| Native RenderGraph | **NOT_IMPLEMENTED** |
| New Graph GPU execution/images, Runtime/FrameLoop | **NOT_RUN** |
| Product, full installed SDK, Linux, Android | **NOT_RUN** |
| Product availability | **EXPECTED_UNAVAILABLE** until F11 |
| F3-PRE-01 integer RenderTarget Clear | **OPEN** |
| F3-PRE-02 Shader include/declaration ordering | **OPEN** |
| F3 authorization | **NOT_GRANTED — STOP for user review** |

F3 must resolve its two recorded prerequisites and establish real Shader/Layout/native pipeline qualification under separate authorization. Logical lifetimes, reuse candidates and scope sharing eligibility are not native synchronization, alias or multi-View execution proofs. Historical R0–R4/F0/F1 results and unrun items are preserved without reclassification.
