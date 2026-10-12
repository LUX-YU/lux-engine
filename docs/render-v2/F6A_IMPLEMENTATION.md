# F6.a — generated DataSchema implementation

Base: `bb3b665e9f3167f14c3aa172bb394fa384fa16e1`. Entry authority is
[F6_ENTRY_DECISION.md](F6_ENTRY_DECISION.md). This document describes implementation;
only the subsequent independent receipt establishes qualification.

## Implemented boundary

`render_data` is a small static component over `render_core` and the existing
header-only `shader_metadata` seam. It has no Graph, Transport, Vulkan, Runtime,
Scene, logging, Python or Shader compiler runtime dependency. All GPU execution
in this stage is an ABI test using existing native owners, not a DataSet system.
No DataSet, Storage, Feature, plugin ABI or structural multi-owner binding is created.

The real installed lux-cxx generator parses `LUX_META` attributes into its IR.
`data_schema.template` projects that IR; `DataSchema.py` lowers the projected facts.
It does not open or parse C++ source text. Generated metadata has static lifetime.
Nested GPU records use `LUX_DATA_VALUE()` so the existing parser exposes their fields.
`LUX_GPU_FIELD(shape="vec3")` or `shape="mat3"` is explicit; ordinary arrays remain
arrays. Matrix arrays are column-major. Exact spelling is exercised by actual codegen.

```cpp
struct LUX_RENDER_DATA(name="example.light", version=1, scope="scene",
                       authority="cpu", gpu="std430") LightGpu
{
    float LUX_GPU_FIELD(shape="vec4") color[4];
};
```

Author build: link `render_data`, then call `render_data_schema(target header)`.
Optional `SERIALIZATION` adds the existing serialization projection to the same parse.
Unknown options, private/inherited fields, missing annotation, pointers in GPU records,
unsupported shapes, invalid versions and unmodelled storage layouts fail explicitly.

## Representation and compatibility contracts

- Canonical name feeds existing `renderDataTypeId`; no new stable hash primitive.
- Schema version, explicit Wire codec/version and optional GPU ABI are independent.
- CPU-only nontrivial string/vector types have lifecycle facts and typed member access,
  without a fictitious Wire/GPU layout. CPU metadata uses real compiler type traits.
- Wire-only uses existing generated Serialization metadata and JSON codec; the fixture
  provides an explicit versioned envelope and checks missing fields/corruption/version.
  `WireData` means a declared encoding contract, not a new universal serializer.
- GPU scalar set: 32-bit float/int/uint; explicit vectors and column-major square matrices;
  fixed C/std::array arrays and explicitly described nested records; std140/std430.
  Unshaped multidimensional arrays, scalar-block layout, SoA and arbitrary C++ objects
  are deliberately unsupported and rejected, not deferred silently.
- Generated packing writes an exact-size caller buffer, zeroes padding, copies actual
  fields and rejects overlap with the input. It allocates no memory. GPU-authority
  metadata exposes no CPU representation or CPU-authored packing operation.
- Shader declarations and C++ packing use the same IR facts. SPIRV-Cross independently
  checks actual glslc output: types, offsets, array/matrix strides and record size.
- Compatibility first validates descriptors, then compares the complete contract.
  Hash equality never substitutes for full equality. There is no implicit migration.
- Existing `RenderDataDescriptor` remains unchanged. Its mandatory Wire/native-layout
  versions are not fabricated for CPU-only types. No adapter is added without a consumer.

Descriptor views borrow caller-owned metadata only for synchronous algorithms;
generated traits borrow static arrays/strings. These C++ views are not a DLL ABI and
must not be retained from an unloaded plugin; host copying/code pins belong to F6.d.

## Type responsibility inventory

| Type | Actual members and unique responsibility | Owner/thread/failure |
|---|---|---|
| EDataScope/EDataAuthority | finite scope and writer class | value; no state/owner |
| DataField | name and actual C++ type spelling | static or synchronous borrow |
| DataCpuRepresentation | size/alignment and move/copy/trivial facts | value; not GPU ABI |
| DataWireRepresentation | codec name and independent version | borrow/value; no codec owner |
| DataGpuField | expanded field path, scalar, offset, vector/matrix/array shape | value with borrowed path |
| DataGpuLayout | storage, size/alignment, fields, declarations, structural bytes/hash | immutable view; no GPU owner |
| DataSchemaDescriptor | type/name/version, scope/authority, fields, optional representations | synchronous view; validated by free algorithms |
| DataSchema<T> | generated static arrays, descriptor/layout/member access and optional packing | stateless trait, no instances or registry |

Concepts separate described, Wire, GPU and CPU-authored GPU capabilities. Every concept
has positive and rejection consumers. Validation and compatibility are free cold
algorithms returning existing `RenderResult` errors. No new public owning class,
thread, Manager, registry or cache is introduced. Heap OOM remains fatal.

## Source, consumer and ABI impact

| Changed seam | Need / prior consumers / impact / regression |
|---|---|
| Graph PassSchema.py and its CMake dependency | approved extraction of value declaration traversal only; original PassSchema IR still owns resource roles/stages; compare all old fixture C++/GLSL bytes and rerun original Shader/Graph tests |
| Shader cmake/GpuValues.py | shared offline IR traversal; Data adds explicit shape/layout/packing facts; no second parser, no production Graph C++ change |
| Description DataLayout.hpp | neutral generated ABI values; only new Data and offline validator consumers; existing description aggregate/Math/Script untouched |
| Shader DataValidation.hpp/.cpp, CMake | cold SPIRV-Cross validation of Data record ABI; no native Layout/PSO or existing runtime contract change; actual reflection tests and native regressions retained |
| Data module | new contracts/free validation; generated Golden and installed consumers; no existing production consumer changed |
| bootstrap F6Data.cmake | real generator/GPU/Meta/SDK wiring; original projection test source and templates reused, no Meta aggregate dependency |

Existing Pass generation output equivalence is checked from the baseline script and
same recorded IR. Hashes and every command are external evidence. The existing Core,
Transport, Graph C++ and Native/Runtime trees are protected by Git object comparison.

## Tests and installation

Golden types: LightGpu, SpriteGpu, CpuOnly, WireOnly, ComplexGpu, ParticleGpu and an
integer GPU record. Real compute/readback verifies float, signed/unsigned values,
std140/std430, nested arrays and matrix packing against CPU arithmetic. This proves
Data representation ABI, not F6.b multi-owner or F6.c storage integration.

Negative tests invoke the real generator or C++ compiler; no hand-written mock traits.
Layout mutation tests reject offset/stride/shape errors and equal-hash unequal facts.
The warmed pack benchmark records all samples and first-party allocations; it is a
new baseline, not a claim to close historical Runtime performance debt.

The bootstrap exports a narrow RenderDataSDK: data/Core headers and libraries,
required generic error headers, neutral metadata and generator scripts. A copied
source-tree-external consumer finds only installed packages, defines its own type,
generates it, links and runs. Three required shared header prefixes are synchronized
with recorded source hashes; Android build/product SDK remain NOT_RUN.

Full ASan needs an annotation-compatible `serialization_json` static library. The
installed dependency at `bf779515a120350c7c5412c366eea73afdf59ccf` is rebuilt from an
unchanged independent checkout with `/fsanitize=address`. An isolated copied SDK
contains that archive; absolute package paths are mechanically relocated with a
before/after manifest. No annotations are disabled and no third-party source is patched.
The pre-existing compatible SPIRV-Cross ASan package is also pinned in build evidence.

## VTune separation

[F6_VTUNE_PROTOCOL.md](F6_VTUNE_PROTOCOL.md) was frozen before the first capture.
`profile_f6.py` is an opt-in external test harness generator, never a production link.
It derives long-running tests from exact historical source and records all hashes.
The Runtime harness links the unwrapped historical production library; native tests
retain their original two timestamp query submissions (reported separately from
production invocation cost). No unbounded query pool or sample vector is introduced.
A 10-frame native warm-up and 24-frame Runtime warm-up precede meaningful warm samples;
Runtime continues 90 bounded blocks of 240 frames. GPU oracles and zero-allocation
checks remain. Profiling does not replace any p95 qualification or permit Native fixes.

## Remaining responsibility

F6.b owns structural BindingSlot and multiple actual owners; F6.c owns minimal DataSet
and backing readiness/last-use; F6.d owns composition/plugin ABI. None is implemented
or authorized by completing this receipt. F4/F5 debts remain PARTIAL/deferred. No new
GPU particle algorithm, platform WSI support or product cutover is claimed.
