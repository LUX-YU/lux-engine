# F1 — Type and algorithm responsibility inventory

All construction below is cold. Generated metadata has static lifetime; Definition owns the captured values and names.
No frame-time parser, registry, manager, native owner, thread or virtual execution interface is introduced.

| Types | Responsibility and ownership | Boundary/cost |
| --- | --- | --- |
| GraphTexture / GraphBuffer | Separate definition-local numeric identities, borrowed logical references | Core StrongId; no native/global identity or ownership |
| GraphSampler | Logical sampler binding token; the later binding owner resolves native backing | Not a VkSampler and not a new registry |
| PassKey / ShaderKey | Distinct stable author and Shader identities | No SceneCapability/RenderData ID alias |
| EPersistentScope / EExecutionScope / EPassKind | Persistent owner scope, execution scope and pass mechanism are different facts | No runtime scheduling objects |
| TextureDesc / BufferDesc / ImageRange / BufferRange and their enums | Structural resource shape and typed access bounds | Owning values; no native allocation |
| SampledTexture / StorageTexture / TransferTexture | Typed logical texture use with subresource range and optional explicit fallback | Value; no borrowed Params storage |
| UniformBuffer<T> / StorageBuffer<T> | Logical buffer plus range; T supplies the actual Shader element ABI | Template encodes a real resource/layout distinction |
| TransferBuffer / VertexBuffer / IndexBuffer / IndirectBuffer | Non-Shader buffer dependencies | Role determines mechanism; not descriptors by default |
| Attachment / DepthStencilAttachment / ResolveAttachment | Color, depth/stencil and resolve contracts with precise operation/clear facts | Distinct values; integer stencil retained |
| SamplerHandle | Author field carrying a logical sampler binding | No content dependency by itself |
| PassResourceField / PassScalarField / PassShaderContract | Generated readonly neutral field, layout and declaration views | No Render or Shader reflection registry |
| PassSchema<T> | Generated traits, C++ ABI assertions and statically typed member accesses | No handwritten fixture specialization |
| GraphPassParameters<T> | Requires generated capture/contract, version, layout and semantic metadata validity | Compile positive/negative tests; not merely trivially-copyable |
| CapturedParameters | Private cold transaction output, owns uses/bindings/scalar bytes or a structured error | Short-lived value, discarded on failure |
| GraphFieldBinding | Captured identity, resource/sampler, owner/frequency, attachment and layout facts | Definition owns strings and values |
| GraphScalarField | Owning scalar field path, ABI and owner/frequency/stage metadata captured from Schema | No dangling plugin/string view in Definition |
| RenderGraphBuilder | Mutable resource/pass consistency during cold composition | finish() && transfers into the single Definition |
| DefinitionAccess | Non-installed internal construction seam for preserved regression oracles | Same production validator; no public compatibility adapter |
| ScalarLocation | Private short reflection result used by cold Shader validation | Borrows SPIRV-Cross compiler storage only within call |

## Algorithms and provenance

- Existing `lux-cxx` parser/IR and multi-projection CMake API: reused unchanged, installed dependency version recorded in
  qualification. No second C++ or annotation parser.
- Frozen `pass_params_hpp.template` / `pass_params_glslh.template`: preserve one-author input, typed member access and
  scalar offset-check invariants. The Vulkan includes, fixed push prefix, old descriptor writes and owner hierarchy are
  deliberately not migrated. Graph projection is installed in the active Graph directory; Legacy is never a codegen input.
- `RenderGraphDefinition::create`: common owning declaration validator extended for ranges and typed author snapshots.
  Existing graph ordering/hazard/lifetime algorithms stay covered by the original oracles; F2 replaces their limited
  whole-resource scheduling. An explicit unsupported-subresource error protects this boundary.
- Existing `.lglsl` declaration classifier/injection is extended for generated contract input and storage image formats.
  Pass-specific input rejects authored physical bindings. Final allocation/relocation belongs to F3.
- Existing SPIR-V reflection is reused. Descriptor array counts now use `type_id` rather than stripped `base_type_id`;
  buffer NonWritable flags include block decorations. These are demonstrated by actual compiled shader fixtures.
- `validatePassSpirv`: cold contract reconciliation of descriptor identity/kind/count/access, image shape/format/stage,
  scalar type/offset/coverage and storage stride. SPIRV-Cross is private. Bad allocation is fatal; other compiler exceptions
  are contained and returned as a cold error. No error registry is added.

## Deferred responsibilities, not implied passes

Full C0–C8, versioned subresource scheduling, culling/condition proofs and complete scope legality: F2.
Actual LayoutPlan, shared-owner shape aggregation, native descriptor/PSO budgets and relocation: F3.
Native Graph recording, physical resources and GPU evidence: F4. Runtime/frame/Scene/Feature/plugin duties remain later.
A same-named Shader interface is not a complete native plan cache key. No F1 test claims GPU output or an advanced algorithm.
