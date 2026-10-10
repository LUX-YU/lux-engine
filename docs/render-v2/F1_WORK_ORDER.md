# F1 — Typed Authoring / PassSchema / Shader contract

Authorization: user's F1-only work order. Base `00ba4075ada74f711fd08300b5f45f2f4ce2150f`.
Frozen V1: `a669409a289a6fa4092f21176397795b1cdb7f3e`. Product: `EXPECTED_UNAVAILABLE`.

## Scope

Allowed: `modules/function/render/graph/**`, neutral Shader metadata/build closure under
`modules/resource/description/**`, existing `engine/toolchain/shader/**`, bootstrap and `docs/render-v2/F1_*`.
Core, Transport, Vulkan Foundation, Legacy, consumers, product CMake, FINAL package and historical reports stay read-only.
No Meta module or lux-cxx modification is required. No Runtime, Scene, native Graph or frame execution is introduced.

## Implementation contract

One existing `lux_add_codegen_job` parses each author header with marker `luxpass` and included marked records enabled.
The existing generator parses annotations and C++ types/offsets/template arguments. `pass_schema.template` projects its
IR and parsed annotation facts. `PassSchema.py` lowers that IR into typed C++ Schema and `.lglslh`; it never reads C++
source, tokenizes C++, parses annotations or registers runtime reflection. Both artifacts have the same input IR.
Unsupported shapes, conflicting roles and unknown annotation keys fail generation. C++ assertions independently check
Clang-reported host size/alignment/offsets against MSVC. Actual glslc SPIR-V is reflected by the existing Shader toolchain.

`RenderGraphBuilder` is the public authoring path. Params are inspected only in cold construction. Owning Definition
captures resource uses, field identities, sampler values, attachment operations and scalar bytes. Temporary Params are
not retained. The previous direct-array factory is private; the regression oracle accesses the identical validator
through a non-installed test/internal header. There is only one Definition and one resource-use representation.

F1 validates declarations and range bounds/overlap. Full versioned subresource scheduling remains F2. The retained R3
compiler accepts proven whole-resource declarations and explicitly rejects partial-resource scheduling; it cannot
silently report HZB mip hazards as proven. R3's 300 random graph oracle and 10,000 frame-binding reuse tests remain.

Shader contract metadata lives in a neutral header. Graph links Core only and includes this header without linking
Description's Math/Script/Asset aggregate. The Shader toolchain uses a separate header-only boundary and SPIRV-Cross;
it is a build-time consumer, not a dependency of production Graph. Provisional shader compiler locations are generated,
not author assignments. F3 LayoutPlan remains the authority for final physical bindings and device budgets.

## Grammar frozen for this stage

- Existing `LUX_PASS_PARAMS()`, `LUX_PASS_SCALARS()` and `LUX_RESOURCE(...)`; one root per author header.
- Resource keys: `role`, `scope=scene|feature|pass_local`, `frequency=static|frame|draw`, `required=true|false`,
  `semantic`, `for`, `dimension`, `format`, `stages` (vertex=1, fragment=2, compute=4; union allowed).
- Resource roles include sampled/storage images, sampler, uniform/storage buffers, color/resolve/depth-stencil,
  transfer source/destination and vertex/index/indirect/input attachment dependencies.
- `for` identifies sampler pairing or a resolve source attachment. Optional resources require an actual typed fallback;
  absent fallback is an error, not a null descriptor.
- Native block values are public scalar/record/fixed-array values. Float/int/uint are 32-bit. Nested field paths and
  fixed arrays are reflected. SSBO elements form runtime-sized shader arrays with checked C++ element stride.
- Pointer/reference fields, bitfield layouts, unsupported scalar/vector/matrix forms, undeclared resource roles and
  unsupported array forms fail explicitly; there is no blind memcpy path for an unknown Shader ABI.
- Scalar fields additionally accept `role=push_constant`, `scope`, `frequency` and `stages`; unsupported keys fail.
- Scalar push offsets derive from the actual author layout; no fixed V1 8-byte prefix or assumed 128-byte device budget.
  F3 must apply the actual device push-constant and descriptor budgets before creating native plans.

## Qualification

Commit implementation I first. Validate tracked snapshot, then independently clone I with no hardlinks or local patches.
Two full bootstrap builds (`all -j 4 -- -k 0`), second no work; preserve prior 59 tests and add F1 tests.
Check generated IR/HPP/GLSL/SPIR-V hashes, positive and negative reflection, public-header and compile rejection tests,
File API/compile commands/Ninja source/include/link/codegen closure, owned snapshot lifetime and original user changes.
Synchronize modified module public headers to Debug/RelWithDebInfo/Android include prefixes with hashes.
Record allocation/timing evidence separately for cold construction and existing stable bindings.

W18 and F1 portions of W01/W15: authoring, generated declarations and real shader compilation/reflection only.
I38 optional Feature ops remain F6. Native descriptors, LayoutPlan/relocation, GPU draw/readback, Runtime, full installed
SDK and product qualification remain later-stage obligations. Existing R4 tests are regression runs only, not new F1 GPU
qualification. Historical verdicts are never rewritten.

Verification commit V may only add the qualification report. Push I/V, then STOP; F2 requires new user approval.
