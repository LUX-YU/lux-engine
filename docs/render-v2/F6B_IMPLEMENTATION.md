# F6.b implementation — structural multi-owner binding

Approved base: c6400f4d97af453da1fa2815356c9fea477458ab. Initial implementation: fb229ba44fa8211c19e1eefd2717f0f7c3f0e972. The final test-completeness commit is its child. F6.a comparison I:
bf3223038d415ed6912dcea04dc8f4d195becd3e. This document describes implementation and development evidence;
only the later independent F6B_VERIFICATION establishes qualification.

## Contract and actual path

Generated DataSchema -> generated PassDataContract -> real SPIR-V ABI validation -> compileLayout
with explicit field-path OwnerAssignment -> relocation/NativeShaderProgram -> existing cold native
recipe assembly -> fixed BoundDescriptorSets/Invocation -> Vulkan GPU.

Three fixed assignment fields are removed. Arbitrarily many same-category owners are supplied as
cold spans; full shapes and exact generated field association are retained by the existing plan.
Physical Set allocation remains solely compileLayout's job: the new fixture has four owners in two
sets, not one set per DataSet. Full unused fields remain part of owner compatibility and device budgets.
An explicitly supplied complete owner may have no active field in one complementary Shader, as the
original F3 full-shape contract requires; unknown/duplicate *field assignments* are rejected.
No instance ID, backing handle, Camera, revision or frame serial enters structural identity.

The existing lux-cxx parser projects marked Data records and Pass parameters. Its marker match is a
substring filter; the Pass job now selects `lux` to include both `luxpass` and `luxdata` records. The
original Pass template still selects only Pass roots. GPU field generation uses the existing common
GpuValues algorithm and static-asserts the actual DataSchema identity. No parser or registry is added.
Runtime-sized SSBO arrays validate reflected record stride and every field, not C++ sizeof(T).
MaterialGpu's CPU size and padded std430 stride intentionally differ. Schema validated=true does not
replace Shader reflection. A cold artifact identity encoding revision includes complete Data ABI bytes.

## Focused repairs and consumers

The preflight equal-basename CMake job failure is repaired with explicit OUTPUT_NAME plus a hashed
job/output identity. Two real `a/Types.hpp` and `b/Types.hpp` generate independently in one installed
consumer. Generated includes point to the actual author file, never an ambiguous basename. These are
consumer-local build products, not prebuilt portable headers. Unmarked nested GPU records are rejected
by the real generator negative test; explicitly marked LightGpu nested records succeed.

The narrow Recipes.cpp change consumes the compiled exact field owner. Its prerequisite cold failure
is recorded in F6B_NATIVE_IMPACT. F2 C++, Runtime, Foundation, Queue, Retirement and Sync2 are unchanged.
Existing F3/F4 test authors migrate to explicit associations without reducing work or oracles.
NativeGraphShared retains its original case and additionally runs split same-category owners.

Bootstrap installs separate RenderBinding and RenderData packages for isolated external consumers.
RenderData still has no Vulkan dependency. RenderBinding exports existing compiled targets and neutral
public closure, not a new production subsystem or F6.d plugin SDK. Only the binding value API is claimed
by that installed consumer; real GPU qualification runs the full production pipeline fixture.

## Development evidence and lifetime

Evidence root: D:/LuxQualification/render-v2-f6b. Latest pre-freeze MultiOwner test passes eight
configurations: one/two Scene identities, four Views, two/three FIF, with/without legal Scene reuse.
One actual Pipeline/PipelineLayout/code is shared within each configuration; every invocation owns
separate descriptors/output/readback. Light main/preview and Material use generated GPU packing and
real reflected ABI. Backings differ by Scene, are swapped after completion, and all 4-float outputs
are independently checked. The final fixture also updates only Scene A MaterialGpu after real completion; Scene B retains its original values. Multi-owner descriptor admission explicitly rejects missing, duplicate, extra and foreign-device values. Scene reuse counts are checked against exact expected counts. Warm
allocation count/bytes are zero. New cold and warm measurements are labelled new baselines with
validation enabled, never compared to an invented old equivalent.

Shared readonly input ready state follows real accepted queue tickets. Donor readback waits for all
accepted consumer borrows, not just the donor's original receipt. An initial test attempted early
readback and correctly received Busy; the fixture was corrected without changing production lifecycle.
Old foreign-device, descriptor rewrite, import alias, last-good and Runtime/WSI cases remain required.
No DataSet, Feature, plugin or storage ownership implementation is claimed.

## Evidence exceptions

The disclosed preflight compiler-intermediate PDB incident is recorded in F6B_NATIVE_IMPACT. Original
archive manifest is unchanged; 3785 other files match. User authorized continuation with the exception.
The first development owner-build failed a test initializer spelling; a subsequent run overwrote that
one full build log before unique log names were enforced. Its compiler error remains in conversation
tool output, not a claimed complete raw log. Later failure logs are retained separately. These exceptions
must remain visible in independent verification; do not claim perfect preservation of every early log.

## Qualification and boundaries

Original 197 named obligations remain; added cases are multi_owner, codegen_identity and reject_nested.
Normal/Full ASan, real GPU/SyncValidation, installed consumers, complete dependencies and source protection
are checked on exact I. Performance protocol is frozen separately before observing formal A/B samples.
Historical F4/F5-C debts remain PARTIAL/deferred. F6.c/F6.d/F7, Android, product integration are NOT_RUN or
NOT_IMPLEMENTED as applicable. No F6.c entry authorization follows from this implementation.

## Implementation file scope

- `cmake/render-v2-bootstrap/CMakeLists.txt`
- `cmake/render-v2-bootstrap/F6Binding.cmake`
- `cmake/render-v2-bootstrap/measure_f6b.py`
- `cmake/render-v2-bootstrap/verify_f6b.py`
- `docs/render-v2/F6B_IMPLEMENTATION.md`
- `docs/render-v2/F6B_NATIVE_IMPACT.md`
- `docs/render-v2/F6B_PERFORMANCE_PROTOCOL.md`
- `docs/render-v2/F6B_TYPE_INVENTORY.md`
- `engine/toolchain/shader/include/lux/engine/toolchain/shader/DataValidation.hpp`
- `engine/toolchain/shader/src/DataValidation.cpp`
- `engine/toolchain/shader/src/PassValidation.cpp`
- `engine/toolchain/shader/src/ShaderAssets.cpp`
- `modules/function/render/data/cmake/DataSchema.py`
- `modules/function/render/data/cmake/RenderDataCodegen.cmake`
- `modules/function/render/data/test/CMakeLists.txt`
- `modules/function/render/data/test/CodegenIdentity.cpp`
- `modules/function/render/data/test/installed/CMakeLists.txt`
- `modules/function/render/data/test/installed/Main.cpp`
- `modules/function/render/data/test/installed/a/Types.hpp`
- `modules/function/render/data/test/installed/b/Types.hpp`
- `modules/function/render/data/test/negative/nested.hpp`
- `modules/function/render/graph/cmake/PassSchema.py`
- `modules/function/render/graph/cmake/RenderGraphCodegen.cmake`
- `modules/function/render/graph/cmake/pass_schema.template`
- `modules/function/render/vulkan/include/lux/engine/render/vulkan/shader/Layout.hpp`
- `modules/function/render/vulkan/include/lux/engine/render/vulkan/shader/Program.hpp`
- `modules/function/render/vulkan/src/graph/Recipes.cpp`
- `modules/function/render/vulkan/src/shader/Bindings.cpp`
- `modules/function/render/vulkan/src/shader/Layout.cpp`
- `modules/function/render/vulkan/src/shader/Program.cpp`
- `modules/function/render/vulkan/test/CMakeLists.txt`
- `modules/function/render/vulkan/test/ComputeBindings.cpp`
- `modules/function/render/vulkan/test/MultiOwner.cpp`
- `modules/function/render/vulkan/test/NativeGraphShared.cpp`
- `modules/function/render/vulkan/test/NativeGraphSupport.cpp`
- `modules/function/render/vulkan/test/NativeGraphSupport.hpp`
- `modules/function/render/vulkan/test/OwnerAssignments.hpp`
- `modules/function/render/vulkan/test/Postprocess.cpp`
- `modules/function/render/vulkan/test/ShaderBenchmark.cpp`
- `modules/function/render/vulkan/test/ShaderColdFailures.cpp`
- `modules/function/render/vulkan/test/ShaderLayout.cpp`
- `modules/function/render/vulkan/test/SharedFixture.hpp`
- `modules/function/render/vulkan/test/installed/CMakeLists.txt`
- `modules/function/render/vulkan/test/installed/Main.cpp`
- `modules/function/render/vulkan/test/shader/CMakeLists.txt`
- `modules/function/render/vulkan/test/shader/F6Light.hpp`
- `modules/function/render/vulkan/test/shader/F6Material.hpp`
- `modules/function/render/vulkan/test/shader/F6Multi.comp.lglsl`
- `modules/function/render/vulkan/test/shader/F6Multi.hpp`
- `modules/function/render/vulkan/test/shader/F6Readback.hpp`
- `modules/resource/description/include/lux/engine/description/DataLayout.hpp`
- `modules/resource/description/include/lux/engine/description/PassContract.hpp`
