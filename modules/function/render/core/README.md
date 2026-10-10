# Render Core (R1)

`lux::engine::render::render_core` is a C++20 static component. Its public headers
live under `lux/engine/render/core/`. The standalone entry is
`cmake/render-v2-bootstrap`; product wiring remains unavailable until R17.

## Contracts

- Semantic IDs use `lux::cxx::StrongId` and `lux::cxx::Fnv1a64`, with zero reserved
  for invalid. Names are exact UTF-8 byte strings. No normalization, registration
  order or local route index participates in identity. Compute IDs at assembly.
- `RenderDataDescriptor` describes native size/alignment and explicit nonzero wire
  and layout versions. It contains no domain payload or lane/reply contract.
  A native layout is not a portable serializer. Registration in R2 must validate
  the record and its compatibility, then validate transport-specific contracts.
- `FeatureDescriptor` and `SceneCapabilityDescriptor` contain only static
  metadata. Capability lists reject null and repeated IDs within each list.
  Provider resolution, cycles, optional effects and execution belong to later
  stages. A feature may both provide and require a capability; Core does not
  resolve whether a particular composition is satisfiable.
- Descriptors are borrowed assembly input records, not live semantic objects.
  Their name/span storage must remain alive while read, including plugin storage.
  Validation is allocation-free and does not mutate either input or publish state.
  `validateCompatible` compares supplied metadata; it does not validate records.
- Handles reuse `lux::cxx::SlotKey`: they borrow identity, never own resources.
  Null checks do not prove liveness. Only the owning registry can check generation
  and domain membership; equal numeric keys from different owners are not portable.
  Core adds no allocator, manager, registry or generation-rollover policy.
- Target semantics use the same stable ID primitive. The initial built-in names
  are scene color and depth; target kind distinguishes offscreen/presentation.
  `DeviceCaps` is only a value projection of two common limits. Zero reports no
  capacity; no device or backend object exists in R1.

## Dependencies and errors

Core links only the real `lux-cxx` core/container/algorithm/compile_time interface
targets. It reuses the tracked generic `modules/core/error/include` value and
descriptor headers as an explicit public build include dependency. It does not
link the error registry or its synchronization service. The existing error IDs
and numeric argument structure are retained; no second error registry is added.
Hosts can pass `renderCoreErrorDescriptors()` to the common registry once during
assembly. Validation failure paths only construct predeclared IDs and numbers.
There is no terminal/logging outlet in Core.

`SlotKey` currently shares its upstream header with `SlotMap`, and the stable
identity header also declares owning identity forms. Those upstream headers pull
some STL container/memory declarations. R1 does not duplicate their primitives
to trim that cost, instantiate an owning container in production, or change
lux-cxx. MSVC STL internal threading declarations are not Render thread APIs.

The component records its future install include location, but this bootstrap
does not install a package or qualify an installed SDK. The generic error value
headers must be included in the eventual installation closure in R16/R17.

## Checks and limits

The default `all` build compiles the real library, each of five public headers in
an independent translation unit, and two positive consumers. CTest exercises
identity/layout/version checks, metadata compatibility, structured errors,
capability lists and stale generations using the actual upstream SlotMap owner.
Six compile-negative consumers test cross-type IDs/handles and inaccessible
Vulkan, Scene, Runtime and legacy public includes using the real Core target.
Their failed builds are intentional and their logs are written outside source.
The matching positive consumer prevents a broken baseline from counting as a
successful negative test. Run CTest serially; probes invoke the build tool.

`verify_r1.py` separately audits the generated CMake File API, actual compiler
header dependencies, linker inputs, the frozen tree, and permitted source paths.
These checks supplement the compile probes; an unavailable include alone is not
a proof of the complete dependency graph.

R1 is CPU-only. No Vulkan, queue, thread, Scene/ECS, route, Feature execution,
domain schema, rendering, installation or GPU qualification is implemented here.
No V1 historical test outcome is changed. Stop for review before R2.
