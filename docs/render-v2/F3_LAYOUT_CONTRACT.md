# F3 Layout, native candidate and binding contract

Qualification is recorded only in `F3_VERIFICATION.md` at its verification commit. This document defines
the implementation contract; development measurements do not qualify it.

## Authority and cold placement

The generated `PassShaderContract` supplies field types, arrays, stage masks, owner category, block
stride and scalar ABI. `declareOwnerShape()` takes the owner's **complete** schemas and merges equal
semantic fields with a union of stages; conflicting declarations fail. It never derives an owner shape
from one shader's reflected subset. Stable owner IDs reuse Core's `StrongId` and existing FNV primitive.

`compileLayout()` validates assignments, complete owner identity/revision, field compatibility, descriptor
counts, flags and push ranges. Scene/Feature owners share set 0; Pass-local fields use set 1 when shared owners exist, otherwise set 0.
Owners and fields have deterministic ordering; empty unused sets are not fabricated. Final set/binding
and dynamic-offset order are owned solely by the immutable `LayoutPlan`. Historical canonical slots are
only provisional relocation inputs, never a competing final binding authority.

All members of every complete shared owner count against device budgets, even when the current shader
uses only a subset. Checks cover bound sets, per-stage/per-pipeline sampler, sampled/storage image,
uniform/storage buffer and input attachment limits, dynamic descriptor limits, total stage resources,
push range bounds/overlap and alignment. `queryLayoutCaps()` records actual physical limits and descriptor
indexing support. The Device factory does **not** enable descriptor indexing; requested nonzero optional
binding flags fail explicitly. Physical support does not grant permission to use a disabled feature.

The complete layout identity is compared field by field. Native identity additionally includes exact
logical cache identity, selected pass, compiled asset/variant/schema/binaries, relocated binaries, fixed
graphics or compute state, target formats/samples, device properties and the actual `VkDevice` scope.
Camera/frame/scalar/clear values stay in invocation storage. No digest alone permits reuse.

## Ownership and publication

`NativeShaderProgram` composes existing R4 move-only owners. Destruction order is pipeline, modules,
pipeline layout, descriptor set layouts. It borrows Device, owns copied identity and all native candidate
backing, and never retains a catalog pointer. Each fallible creation either returns the complete candidate
or unwinds its private partial owners. The caller publishes only a successful candidate; failure leaves
the old candidate and its output intact. A successful replacement transfers the old complete candidate
to existing R4 retirement with its last submission serial. No per-resource DeviceWaitIdle is introduced.

`BoundDescriptorSets` cold-allocates one pool and the full required sets, then writes every descriptor
array element exactly once. Missing, duplicated, wrong-owner/type/range values fail before publication.
It borrows the program layout and backing resources until the last GPU use; owners must be retained
together. It has no update-in-flight method, lazy growth or runtime semantic lookup. Temporary program
borrows are rejected by the deleted rvalue overload. Cross-thread publication/scheduling remains F5.

Cold `OwnerDescriptorValue` names select full-owner fields. The resulting fixed recipe stores only native
sets, pipeline layout, dynamic range limits and push ranges. `bind()` validates bounded dynamic offsets
and exact invocation scalar storage, then records descriptor/PC commands. It does not parse schemas,
lookup names, allocate, create pipelines or reflect SPIR-V. Buffer byte intervals describe the shader
view; the supplied physical base plus dynamic offset selects its backing slice. R4 checks physical
bounds/alignment; F4 will own the logical-resource-to-physical-allocation mapping and synchronization.

## Explicit supported boundary

The existing R4 Image mechanism remains 2D, one mip and layer; sampled/storage descriptor creation
rejects broader views rather than silently under-binding them. Supported generated storage formats are
`rgba32f`, `rgba16f`, `r32f`, `rgba8`. Neutral format identity remains `rdesc::ETextureFormat`; `Format.hpp`
only maps it exhaustively to native constants. F4's complete resource plan still owes general mip/layer,
history, alias, queue and synchronization support. Graph/HZB authoring ranges remain fully represented.

Graphics state covers vertex input, topology/culling, dynamic viewport/scissor, depth/stencil, blend,
target formats and sample count. Unsupported optional features, invalid formats and incompatible shader
interfaces fail cold. Specialization overrides are not exposed: the exact cooked SPIR-V contains the
default specialization values and is part of variant/native identity; native creation passes no override
map. A different compiled define/source/binary forms a different variant. No claim is made for arbitrary
runtime specialization, mesh/tessellation/geometry stages, interface blocks or 64-bit stage IO.

Foundation remains `render_vulkan` (Core/Vulkan/VMA). Graph-aware cold creation belongs to
`render_vulkan_shader_compiler` in the same module; the neutral Graph does not link it. The hot-link probe
requires actual `Bindings.cpp.obj` but excludes Program/reflection/relocation/catalog objects and SPIR-V
DLL imports. Installation-header synchronization is not installed SDK qualification.
