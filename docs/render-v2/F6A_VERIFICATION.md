# F6.a independent verification

**F6A = PASS** at Implementation `bf3223038d415ed6912dcea04dc8f4d195becd3e`. VTune diagnosis is separately
**INCONCLUSIVE**, not a stage blocker or a historical performance waiver.

Approved base: `bb3b665e9f3167f14c3aa172bb394fa384fa16e1`.
Implementation history: `a141453df96c8a4e045bddb40c15cf085f815085` introduced the code;
`ae90566f885a63087ad2719017ac211952841d00` corrected the verifier's UTF-8 Git paths;
final I retains the pack workload against compiler elimination. No production fix
was made inside a qualification clone. V is the report-only commit containing this
receipt and F6_VTUNE_DIAGNOSTIC.md; its exact SHA is recorded after commit in the
external delivery receipt and final delivery message (no self-referential hash).

## Isolation and evidence

Qualification source: `D:/LuxQualification/render-v2-f6a/qualification/bf3223038d415ed6912dcea04dc8f4d195becd3e/source`.
Created with `clone --no-hardlinks --no-checkout`, detached exact I, autocrlf=false.
Tracked snapshot passed; no alternates, maximum Git object link count 1; source clean
before and after qualification. Builds, SDKs, consumers and evidence are outside it.

Evidence root: `D:/LuxQualification/render-v2-f6a` (actual retained location, not an assumed archive).
`evidence-manifest.json` SHA-256: `99377492edd87638546ea8f389221a25ea0a91185a4516221c8ccd521fd0a8dc`.
`qualification/bf3223038d415ed6912dcea04dc8f4d195becd3e/final-audit.json`, `artifact-identities.json`, Normal/ASan
`*-closure/closure.json`, CMake File API, compile commands, Ninja includes/commands,
CTest JUnit/LastTest logs and every command's exit code are retained. Generator IR,
C++/GLSL/SPIR-V, EXE/PDB/library hashes, original failures and VTune databases remain.
`delivery-inputs.json` lists all changed Git blobs; complete file list is below.

## Executed results

| Qualification | Normal | Full ASan |
|---|---|---|
| bootstrap all, `-j 4 -- -k 0` | PASS | PASS |
| second all build | no work | no work |
| CTest | 197/197 PASS | 197/197 PASS |
| original test obligations | 175 preserved and run | 175 preserved and run |
| new obligations | 22 PASS | 22 PASS |
| real source/include/link/codegen closure | PASS | PASS |
| source-tree-external installed consumer | generate/build/link/run PASS | generate/build/link/run PASS |
| generated Data GPU ABI/readback | PASS, validation/sync validation 0 error | PASS, 0 error |
| warmed generated packing | 0 allocations / 0 bytes | 0 allocations / 0 bytes |

The 22 additions include original `meta.projection_compatibility`, Data schema/Wire/GPU/
pack tests, nine real codegen rejections and eight real C++ concept/dependency rejections.
Independent public-header TUs are compiled by all. Original Graph random/version/scope/
binding tests, Transport, Native/Runtime/Presentation/WSI regression tests were actually
run here, not relabelled inherited evidence. Their benchmark smoke outputs do not
reopen or close the deferred statistical p95 experiments.

`meta.projection_compatibility` uses unchanged original source and templates.
The installed lux-cxx parser consumes marked C++ and emits real IR/projections.
The independent consumer declares its own External type through the installed package.
Its Ninja commands/includes contain only its own source, isolated installed SDK,
installed dependencies and compiler headers; no engine source/private/old product SDK.
Normal and ASan link the actual installed Data/Core archives.

The shared GPU value generator extraction preserves all 45 recorded original Pass
IR outputs or explicit rejections (`pass-final-equivalence.json`). New ABI checks use
real glslc/SPIRV-Cross, not the generator's own layout calculation as an oracle.
CPU-only nontrivial objects remain valid; Wire has explicit versioned JSON encoding;
GPU authority does not acquire a fake CPU mirror or CPU-authored pack method.

## GPU and performance meaning

Device: NVIDIA GeForce RTX 4070 Ti; raw driver 2480242688 (591.86), Vulkan API 1.4.325;
SDK/glslc 1.4.304.0; MSVC 19.44.35228, C++20 RelWithDebInfo. Existing Khronos validation
and synchronization validation are enabled by the Foundation instance fixture.
Loader naming warnings for GalaxyOverlay layers are retained, not counted as zero
warnings or suppressed. The fixtures assert zero validation errors after teardown.

Real generated record packing, shader declarations and reflection passed for LightGpu
(std430), SpriteGpu (std140), nested/array/matrix ComplexGpu (std140), and signed/unsigned
IntegerGpu. Independent arithmetic readback totals were respectively 36, 21, 190, 14.
Offsets, scalar/vector shape, matrix/array strides and record size are separately
compared against actual SPIR-V. CPU tests additionally reject corrupted stride/range,
layout and hash-equal incompatible descriptors. These are aggregate numeric oracles,
not exhaustive per-component shader semantics.

The ABI fixture supplies only a test I/O PassShaderContract (input/output buffers)
to the existing emitter; Data record fields/layout are genuinely generated. Physical
set/binding values come from that emitter. It uses existing R4 native owners/queue
and a real shader-to-host Sync2 dependency. It does not claim a new generated
DataSchema-to-MultiOwner PassParams integration, DataSet or Feature implementation.
Those consumer responsibilities remain F6.b–d.

The 1,000-sample pack record reports Normal p50/p95/max =
0/100/100 ns;
ASan = 200/300/400 ns.
Raw samples are preserved. Normal timing is quantized at 100 ns: a zero median is
below clock resolution, **not zero execution cost**. There is no equivalent old
DataSchema baseline or claimed speedup. The final object disassembly retains indirect
pack calls and the generated body (`qual-bf3223038d41-pack-disassembly.log`). Allocation
checks are the meaningful warmed contract; finer throughput is NOT_QUALIFIED by this
microsecond-scale clock. No historical 5% gate is reclassified.

## Compatibility, protection and limitations

Full ASan initially exposed incompatible installed `serialization_json` STL annotations.
The unchanged dependency commit `bf779515a120350c7c5412c366eea73afdf59ccf` was rebuilt with
ASan into an isolated SDK, with only mechanical relocation of absolute package paths.
No annotations were disabled, no third-party source/ABI was patched. The existing
compatible SPIRV-Cross ASan package and every command/input hash are retained.

The default CTest passed-output cap truncated the initial JUnit; a subsequent test-list
query also replaced LastTest. Both configurations were rerun on the same final I with
verbose output and a 16 MiB per-test cap. Complete `*-ctest-full.xml`, command logs and
`*-LastTest-full.log` are preserved; no tests or unfavorable results were removed.

The first qualification's verifier rejected Git-quoted Chinese paths; that failed log
and initial I remain. It was corrected in construction with NUL-delimited Git paths.
The earlier inline pack benchmark was superseded by a volatile function-pointer call,
with all old samples retained. Neither correction was a hidden qualification patch.

PASS: 719 Legacy source blobs/modes/sizes; protected Core/Transport/Graph C++/Vulkan/
Runtime Git objects; FINAL/RD2 and all historical reports. PASS: both protected user
workspace HEADs, branch/status, staged/unstaged diffs, untracked lists and all seven
original byte hashes. New public Data/neutral metadata headers were synchronized to
Debug/RelWithDebInfo/Android header prefixes with source hashes (`install-sync.json`).
That is header hygiene, not Android or full product SDK qualification.

`render_data` is a real static library over Core and neutral metadata; no Vulkan,
Graph, Transport, Runtime, Scene, Script/Math or compiler-tool runtime dependency.
No new owner, worker thread, manager, registry or persistent resource authority.
Production Native/Runtime ABI and hot-path source are unchanged. The profile harness
is opt-in test-only over frozen historical production; it is not linked into Data or
Runtime production. New type responsibilities and exact consumer/ABI impact are in
[F6A_IMPLEMENTATION.md](F6A_IMPLEMENTATION.md).

NOT_RUN: Android/Linux builds, full root/product and full product SDK. NOT_IMPLEMENTED:
F6.b structural multi-owner binding, F6.c DataSet/backing, F6.d composition/plugin SDK,
F7 storage, particle algorithms. PMU/uarch not available; profiler scope and limitations
are in [F6_VTUNE_DIAGNOSTIC.md](F6_VTUNE_DIAGNOSTIC.md).

## Changed implementation files

```text
cmake/render-v2-bootstrap/CMakeLists.txt
cmake/render-v2-bootstrap/F6Data.cmake
cmake/render-v2-bootstrap/profile_f6.py
cmake/render-v2-bootstrap/verify_f6a.py
docs/render-v2/F6A_IMPLEMENTATION.md
docs/render-v2/F6_ENTRY_DECISION.md
docs/render-v2/F6_VTUNE_PROTOCOL.md
engine/toolchain/shader/CMakeLists.txt
engine/toolchain/shader/cmake/GpuValues.py
engine/toolchain/shader/include/lux/engine/toolchain/shader/DataValidation.hpp
engine/toolchain/shader/src/DataValidation.cpp
modules/function/render/data/CMakeLists.txt
modules/function/render/data/cmake/DataSchema.py
modules/function/render/data/cmake/RenderDataCodegen.cmake
modules/function/render/data/cmake/data_schema.template
modules/function/render/data/include/lux/engine/render/data/Annotations.hpp
modules/function/render/data/include/lux/engine/render/data/Schema.hpp
modules/function/render/data/src/Schema.cpp
modules/function/render/data/test/Benchmark.cpp
modules/function/render/data/test/CMakeLists.txt
modules/function/render/data/test/Golden.hpp
modules/function/render/data/test/Gpu.cpp
modules/function/render/data/test/Probe.cpp
modules/function/render/data/test/Reject.cmake
modules/function/render/data/test/Schema.cpp
modules/function/render/data/test/Wire.cpp
modules/function/render/data/test/installed/CMakeLists.txt
modules/function/render/data/test/installed/External.hpp
modules/function/render/data/test/installed/Main.cpp
modules/function/render/data/test/negative/Main.cpp
modules/function/render/data/test/negative/authority.hpp
modules/function/render/data/test/negative/borrow.hpp
modules/function/render/data/test/negative/layout.hpp
modules/function/render/data/test/negative/missing.hpp
modules/function/render/data/test/negative/private.hpp
modules/function/render/data/test/negative/scope.hpp
modules/function/render/data/test/negative/shape.hpp
modules/function/render/data/test/negative/version.hpp
modules/function/render/data/test/negative/wire.hpp
modules/function/render/graph/cmake/PassSchema.py
modules/function/render/graph/cmake/RenderGraphCodegen.cmake
modules/resource/description/include/lux/engine/description/DataLayout.hpp
```

## Stage state and stop

```text
F6A = PASS
VTUNE_DIAGNOSTIC = INCONCLUSIVE
F4_COLD_DEBT = PARTIAL — deferred
F5C_RECORD_SUBMIT_P95 = PARTIAL — deferred
F6_OVERALL = INCOMPLETE
F6B = AWAITING_USER_REVIEW
F7 = NOT_AUTHORIZED
V2_PRODUCT = EXPECTED_UNAVAILABLE
STOP
```

F6.b may be reviewed using the generated schema/ABI evidence, but requires the next
explicit review release and its own Native source/consumer/ABI/performance impact
contract. This receipt does not authorize changing LayoutPlan or the three-slot model.
