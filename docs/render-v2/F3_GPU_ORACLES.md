# F3 GPU oracles and regression mapping

All results below are required checks, not predeclared PASS. The locked-commit report and raw archive
provide actual outcomes. Tests use production generated schemas/emitter/validator/layout/candidate
creation and the real Vulkan driver. Test-only command recording is one-shot; it is not a Native Graph.

| Contract | Executable source / actual oracle |
|---|---|
| G01 / W01 | `Postprocess.cpp`: nonuniform HDR pixels; exposure tone mapping; clamped 3x3 neighborhood Blur; two-input weighted Composite and additive cooked variant. Both individual and chained GPU outputs compare with CPU arrays. Float tolerance 0.00002; sRGB tolerance 1.1/255. Inputs, output and actual relocated SPV/layout facts are saved. |
| G02 / G04 | `ComputeBindings.cpp`: typed UBO, readonly input SSBO, two writable SSBO descriptor-array elements, storage image, PC count. Real dynamic UBO/SSBO offsets, exact readback and cold wrong backing/range/array/offset rejection. |
| G03 / W15 | `SharedOwners.cpp`: Scene and Feature each declare their full two-field owner shape from complementary generated VS/FS schemas. Two pipelines retain all four bindings and read different subsets. Exact output; stable layout comparison; both final SPV/layout sets saved. |
| G05 / F3-PRE-01 | `AttachmentClear.cpp`: actual rendering attachment loadOp CLEAR with no draw required, followed by copy/readback. R32_UINT 16777217 and UINT_MAX, RGBA UINT, SINT -1/INT_MIN/INT_MAX, float HDR and sRGB. CPU Authoring/Invocation negatives are `graph/test/ColorClear.cpp`. |
| G06 | `RasterState.cpp`: real depth/stencil accept/reject and alpha blend, sample counts 1 and 4 with resolve; expected RGBA (0.75,0.5,0,0.5). Test-local barriers and resolve only, no general scheduling implementation. |
| G07 / I42 native part | `ShaderCandidate.cpp`: ten injected native failures unwind candidates while old output survives; successful replacement gives new output. Two real in-flight submissions and R4 retirement. Private seam postpones CPU fence observation, not actual GPU execution; no destruction before real completion. ImageView/Sampler/GraphicsPipeline move/destruct checks included. |
| G08 cold | `ShaderLayout.cpp`, `ShaderColdFailures.cpp`, `ComputeBindings.cpp`: queried limits and deliberately reduced snapshots, full-owner budgets, dynamic alignment, count/type/owner/flag/PC failures. Invalid inputs are not GPU workloads. |
| G09 cold | `ShaderColdFailures.cpp`, `ShaderAssets.cpp`: malformed/truncated/duplicate/missing/grouped/aliased SPIR-V decoration, entry stage, schema ABI and stage IO mismatch. Original and final valid binaries receive production spirv-val and re-reflection. |
| F3-PRE-02 | `graph/test/authoring/F3Includes.py`: five positive and nine negative emitter/glslc/spirv-val/reflection cases, transitive includes, type-before/helper-after marker and unchanged Legacy input. Original multistage tests remain. |

GPU correctness fixtures enable Khronos validation and synchronization validation, preserve warnings,
and require zero errors after native teardown. Performance uses a separate uninstrumented instance;
that instance never substitutes for correctness qualification. R4 original native/fault/retirement tests
remain unchanged regression obligations. Device features/limits, loader and driver identity are archived.

## Performance protocol

`ShaderBenchmark.cpp` records 100 raw cold samples after five warmups for full layout, relocation plus
validation, complete candidate (including cook/file IO), isolated native graphics PSO create/destruct,
and descriptor pool/sets/write. Every cold group reports allocation count and bytes. These are new F3
baselines, not percentages against V1. The fixed recipe records 1,000,000 calls in 1,000 batches after
10,000 warmup calls and requires zero first-party C++ allocations. Driver C allocation and VRAM peak
are not measured by C++ new instrumentation. GPU timestamps cover a shared 2x2 draw, barriers and copy;
their allocation-census flag is false. They do not claim isolated shader throughput.

F2-FIX Graph binding runs seven alternating before/after pairs with identical checksum/work, fixed CPU
affinity and equal above-normal (not realtime) priority, one million operations and 10k warmup per run. Median p50/p95 regression above 5% requires
investigation/STOP; no discarded runs. The unchanged 64/128/256/512-pass compiler benchmarks retain
versions/hazards/dependencies/work counts and provide cold scaling evidence. Their original 5/20 samples
make each run's p95 equal its maximum; qualification pools every raw sample from 35 alternating pairs
per size and retains the per-run statistics. No samples are discarded. Full normal and ASan
CTest retain all original 124 names and add 13 F3 tests. ASan uses matching SPIRV-Cross STL annotations.

## Remaining obligations

F3 mechanisms do not close Material I42 (F8), Tonemap/Highlight business Features (F9), full Native Graph
(F4), Runtime/multiple View pacing (F5), external plugin/installed SDK (F6/F11), or product cutover (F11).
No Linux/Android qualification, general queue/barrier/alias compiler or runtime hot reload is claimed.
