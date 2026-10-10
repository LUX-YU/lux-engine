# F3 implementation inventory

Approved base: `c986e69765c9dd222469d4cf5090aa72549966dc`.
Only F3.0–F3.4 are authorized. This file describes implementation scope, not qualification results.

## F3.0 source inventory

All paths below are ALLOWED by the F3 work order.

| Path | Responsibility and evidence |
| --- | --- |
| `modules/resource/description/include/lux/engine/description/Image.hpp` | Unique neutral format-to-clear classification; exhaustive format tests |
| `modules/function/render/graph/include/lux/engine/render/graph/Authoring.hpp` | Closed typed color clear value; no native types |
| `modules/function/render/graph/include/lux/engine/render/graph/Schema.hpp` | Generated capture preserves exact clear category and bits |
| `modules/function/render/graph/include/lux/engine/render/graph/Definition.hpp` | Owning captured clear value |
| `modules/function/render/graph/include/lux/engine/render/graph/Bindings.hpp` | Invocation dynamic clear uses the same author value |
| `modules/function/render/graph/src/Definition.cpp` | CLEAR type/format validation; LOAD/DISCARD ignore unused numeric value |
| `modules/function/render/graph/src/Bindings.cpp` | Per-invocation category validation against existing structural format/load |
| `modules/function/render/graph/test/**` | Generated Params, exact integer preservation, invalid category/range, plan reuse |
| `engine/toolchain/shader/src/lglsl/LglslEmitter.cpp` | Explicit declaration insertion point, version/extension ordering, original stage contract |
| `engine/toolchain/shader/**` tests | Real glslc/spirv-val/reflection positive and negative include fixtures |
| `docs/render-v2/F3_*` | Current work order, source/type inventory and eventual independent qualification |

The existing logical identity already contains texture format and attachment load/store, so it need not
store a redundant clear category. Numerical clear values remain in real invocation data. C3 hazards,
C5 reachability and C7 sharing proofs are read-only; no scheduler or runtime is introduced.

The Graph target still links Core and uses neutral Description headers. The offline emitter/reflection
targets remain build tools. Description's Math/Script aggregate is not a Graph dependency. F3 native
compilation will have a dedicated consumer target inside the existing Vulkan component, leaving the
R4 Foundation target independent of Graph and offline tools.

## F3.1 incremental source inventory

| ALLOWED path | Responsibility |
| --- | --- |
| `modules/function/render/vulkan/include/lux/engine/render/vulkan/shader/Layout.hpp` | Cold OwnerShape values, explicit owner assignment, queried layout caps, immutable LayoutPlan |
| `modules/function/render/vulkan/src/shader/Layout.cpp` | Full-shape derivation, deterministic placement, descriptor/PC budgets, exact identity and diagnostics |
| `modules/function/render/vulkan/CMakeLists.txt` | Dedicated cold shader compilation target; R4 Foundation keeps its prior dependencies |
| `modules/function/render/vulkan/test/**` | Real queried device caps and schema-driven placement, budget/type/owner negatives |

OwnerShape is a complete declaration supplied by its owner; no registry is created. It can be derived
from the owner's complete generated schemas, independently of shader reflection subsets. LayoutPlan
is a validated value with private construction; it owns immutable descriptor/PC placement. Native
resources continue to be owned exclusively by R4 types and their necessary extensions.

Budget reference: Vulkan `VkPhysicalDeviceLimits` and `VkPipelineLayoutCreateInfo` at
https://docs.vulkan.org/spec/latest/chapters/limits.html and
https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineLayoutCreateInfo.html.
Pipeline totals and per-stage totals are checked across all physical sets. Optional descriptor
indexing requests are rejected when not enabled; querying support does not imply enablement.

## F3.2–F3.3 incremental source inventory

| ALLOWED path | Responsibility |
| --- | --- |
| `engine/toolchain/shader/{include/lux/engine/toolchain/shader,src}/PassValidation.*` | Existing validator accepts the unique final location map; original provisional mode remains its default |
| `engine/toolchain/shader/{include/lux/engine/toolchain/shader,src}/SpirvRelocation.*` | Direct decoration-pair relocation, SPIRV-Tools C ABI validation, original/final reflection |
| `engine/toolchain/shader/CMakeLists.txt` | Installed SDK SPIRV-Tools shared C ABI dependency; no new parser or third-party source change |
| `modules/function/render/vulkan/{include/lux/engine/render/vulkan,src}/device/Device.*` | Explicit optional dynamic-rendering negotiation; original default unchanged |
| `modules/function/render/vulkan/{include/lux/engine/render/vulkan,src}/memory/Memory.*` | Existing Image owner supports queried sample count; no parallel allocator |
| `modules/function/render/vulkan/{include/lux/engine/render/vulkan,src}/descriptor/Descriptors.*` | Existing pool adds range-checked typed native buffer/image/sampler writes |
| `modules/function/render/vulkan/{include/lux/engine/render/vulkan,src}/descriptor/ImageBindings.*` | ImageView and Sampler each own one distinct native handle |
| `modules/function/render/vulkan/{include/lux/engine/render/vulkan,src}/pipeline/Pipeline.*` | Existing PipelineLayout accepts validated stage-specific push ranges |
| `modules/function/render/vulkan/src/pipeline/Objects.cpp` | Existing pool move transfers added uniform limit facts |
| `modules/function/render/vulkan/{include/lux/engine/render/vulkan,src}/pipeline/Graphics.*` | Real dynamic-rendering graphics PSO RAII, fixed vertex/raster/depth/stencil/blend/MSAA state |

Native image views borrow Image backing; samplers borrow Device; both must outlive GPU descriptor uses.
GraphicsPipeline adds the previously absent native handle owner without duplicating ComputePipeline,
Device, allocator or retirement authority. Device enables dynamic rendering only when requested and
supported. Optional features not enabled by its factory fail explicitly at cold configuration.

## User review amendment: assets and cumulative evidence

The amended F3 work order restores full W01 (Tonemap/Blur/Composite), maps I09/I10/I11/I12/I42,
limits G06/G07 to native PSO mechanisms, and classifies G08/G09 as cold negative checks.
`F3_CAPABILITY_STATUS.md` is the sole cumulative index, referencing frozen IDs and historical I/V.
It is not a second design or a replacement for immutable qualification reports.

| ALLOWED file / type | Responsibility / ownership / failure / validation |
|---|---|
| `engine/toolchain/shader/include/lux/engine/toolchain/shader/ShaderAssets.hpp` and `src/ShaderAssets.cpp` | Cold compiled Shader Asset/Variant values and bounded directory; no native execution, thread or global registry |
| `ShaderBuildInputs`, `ShaderSourceInput`, `ShaderDefine`, `ShaderStageBinary` | Owning exact cold build facts; caller supplies actual build inputs; no borrowed source memory |
| `ShaderVariantIdentity` | Exact source/dependency/defines/compiler/target/generated-schema/actual-binary equality, not hash-only matching |
| `CompiledShaderVariant` | Factory-validated owning cooked program; existing SPIR-V validator and complete stage reconciliation; invalid input returns diagnostic |
| `ShaderVariantCatalog` | Move-only bounded cold owner of compiled values, single owner thread; borrowed lookup expires on mutation/destruction; replaced/removed values returned to caller |
| `shaderSchemaIdentity()` | Deterministic length-prefixed encoding of existing generated facts for exact equality; no parser, new schema or reflection authority |
| `modules/function/render/vulkan/test/ShaderAssets.cpp` | Real generated Complex SPIR-V; capacity, exact mismatches, missing/duplicate inputs, malformed binary, replace/remove and last-good negatives |

The catalog is deliberately cold and uses linear exact comparison in its fixed entry capacity. It may
allocate owning source/binary values during construction or replacement, never in frame binding. Native
candidates must own their identity/backing independently; retaining a catalog pointer is not a GPU pin.
Material registration and runtime hot-reload services are excluded. I42 whole-row closure remains F8-dependent.

## Final F3 type responsibility inventory

All listed public C++ interfaces are C++20 source APIs, not the deferred plugin C ABI. Value data may be
copied across ownership boundaries; native owners are move-only and require their borrowed Device to
outlive them. Cold construction may allocate; ordinary heap OOM is fatal. Native failures use existing
RenderError including VkResult; offline compiler failures use the existing expected/string boundary.
No type creates a thread. Single-owner mutation and externally synchronized Vulkan submission apply.

| Types / visibility | Category, owner and necessity | Borrow / path / failure / dependency / evidence |
|---|---|---|
| FloatColorClear, SintColorClear, UintColorClear, ColorClearValue / public | Value; invocation owns exact active numeric category; private union preserves generated standard-layout ABI | No borrow; capture cold, read hot; format mismatch is Graph error; neutral Image; ColorClear + G05 |
| OwnerField, OwnerShape, OwnerAssignment / public | Owning field/shape values and typed assignment; full owner schema cannot be inferred from shader subset | Cold; duplicate/mismatch rejection; Core + neutral schema + native descriptor enums; G03/G08 |
| DeviceLayoutCaps / public | Value snapshot of actual limits and support, not a feature-enablement authority | Cold borrowed query input; no device ownership; Vulkan; G08 |
| DescriptorLocation, LayoutBinding, LayoutSet, PushRange, LayoutIdentity / public | Owning final layout facts; exact equality includes every owner field and physical placement | Cold output, immutable program identity; budgets checked before result; G03/G08/G09 |
| LayoutPlan / public | Validated owning result, private factory construction; not a compiler service | No retained borrowed schema; cold diagnostics; compileLayout expected; same component dependencies |
| ShaderLocationType, PassStageInterface, PassDescriptorLocation / public | Owning cold reflection/interface or location values, no SPIRV-Cross public ABI | Reflect algorithms borrow spans only synchronously; expected errors; G09 and original stage program |
| RelocatedShader / public | Owning original/final binary plus mapping and stage; preserves proof input | Cold only; expected compiler diagnostic; SPIRV-Tools C + existing reflection; G09 |
| ShaderSourceInput, ShaderDefine, ShaderBuildInputs, ShaderStageBinary, ShaderVariantIdentity / public | Owning source/compiler/schema/binary values, exact cache equality | No source pointer retained; cold; ShaderAssets tests; no native ownership |
| CompiledShaderVariant / public | Validated cooked value, private construction | Owns every input; cold; same validator; ShaderAssets/G01–G04 |
| ShaderVariantCatalog / public | Bounded move-only cold directory, explicit entry capacity; no global identity service | Borrow until mutation; capacity/not-found errors; replacement returns prior owner; ShaderAssets |
| VertexBinding, VertexAttribute, StencilFace, ColorBlend, GraphicsDescription / public | Fixed native PSO description values, no Shader/Graph registry | Cold validation; device limits/format/features; G06 and cold negatives |
| GraphicsPipeline / public | RAII owner of newly supported VkPipeline kind; existing Device/Module/Layout borrowed | Cold creation; destructor releases one handle; native failure rollback; G01/G06/G07 |
| ImageView, Sampler, SamplerDescription / public | Distinct native view/sampler ownership and cold sampler value; do not duplicate Image backing | Borrow Image/Device; rvalue Image rejected; G01/G04/G07 move/destruct/fault |
| ComputeDescription, VPipelineDescription, NativeProgramIdentity / public | Closed pipeline-state choice and complete owning candidate identity | Cold; exact device-scope/layout/shader/target comparison; G03/G07 |
| NativeShaderProgram / public | Complete composite candidate of R4 RAII owners; no second native hierarchy | Borrows Device, owns identity/modules/layouts/PSO; cold expected factory; G01–G07 |
| BufferDescriptorValue, ImageDescriptorValue, VDescriptorValue, OwnerDescriptorValue / public | Cold borrowed write inputs with typed alternatives; not persistent resource ownership | Backing/program must outlive GPU references; semantic names used only during create; G02/G04 |
| BoundDescriptorSets / public | RAII pool/set owner and fixed record recipe; immutable writes before publication | Borrows native backing/layout; cold expected creation, bounded hot bind; G04/G07/1M allocation test |
| Budget / private Layout.cpp | Cold value accumulator for actual limits; no persistent manager | Stack-only; overflow/limit rejection; G08 |
| Pair / private SpirvRelocation.cpp | Cold positions for both direct decorations; prevents partial in-place matching | Local owning scan storage; malformed input errors; G09 |
| ScalarLocation / private PassValidation.cpp | Existing scalar layout reflection helper extended by the same validator | Cold synchronous reflection; ABI mismatch errors; old and new shader negatives |
| DynamicRange / private BoundDescriptorSets | Fixed alignment and maximum dynamic offset; avoids hot backing lookup | Owned bounded vector created cold; hot range errors; G04 |
| VPipeline / private NativeShaderProgram | Closed alternative owning existing ComputePipeline or GraphicsPipeline | Reverse destruction after fence responsibility; G07 |

New free algorithms are declareOwnerShape/compileLayout/queryLayoutCaps/nativeStages, neutral/native
format conversion, nativeColorClear, reflectPassInterface, relocatePassSpirv, shaderSchemaIdentity and
makeCompiledShaderVariant. Their input borrows are synchronous; returned values own their state. No
persistent algorithm wrapper or Builder is added. `Bindings.cpp` separates non-null invocation validation
into a private ordinary function so added Clear checks do not expand the no-invocation binding path;
all original validation conditions remain. Development comparison logs retain the initial regression and
subsequent measurements. Test-only PendingSharedDraw/TimestampPool and fixture
helpers own one-shot resources, have no production ABI and never become a FrameExecutor.

## Final files and dependency organization

`vulkan/shader/{Layout,Format,Program,Bindings}.hpp` and their `.cpp` files implement cold native
compilation and a separate fixed-recipe object. Existing Foundation remains Core/Vulkan/VMA-only.
Toolchain `{ShaderAssets,SpirvRelocation,PassValidation}` remains the only offline binary/metadata path.
The dedicated static compiler target makes Graph dependencies explicit without changing Foundation's
consumers. `verify_f3.py` checks File API, every actual compiler include, source, library, codegen depfile,
the hot consumer link map/import table and all 124 retained CTest names. No root/product wiring changes.

`F3_GPU_ORACLES.md` maps all new tests and exact numerical/proof obligations. The final verification
report contains the complete Git changed-file list, independent source SHA, generated hashes and public
header synchronization evidence. No manually authored generated fixture specialization is introduced.

## Frozen algorithm provenance

All following paths are below `render_legacy/modules/function/render/vulkan/`, byte-identical to source
`a669409a289a6fa4092f21176397795b1cdb7f3e`; the source manifest supplies original path/blob/mode/size.

| Frozen reference | Retained invariant / explicit F3 difference |
|---|---|
| `src/gpu/pipeline/SpirvPatcher.cpp`, `sinclude/lux/engine/render/gpu/pipeline/SpirvPatcher.hpp` | Collect original paired positions before rewriting either word. V2 owns output, rejects grouped/ambiguous modules, and validates/re-reflects both ends; no legacy binary dependency or in-place public mutation. |
| `sinclude/lux/engine/render/gpu/pipeline/EngineSetShapes.hpp` | Shared owner layout must be complete rather than a reflected subset. V2 obtains full typed owner schemas and deterministic placement, not old engine-specific fixed slot ownership. |
| `src/gpu/pipeline/StandardPipelineLayoutBuilder.cpp`, corresponding sinclude header | Native layout assembled cold, checks stage visibility and descriptor shape. V2's LayoutPlan alone supplies locations and device budgets; no giant pipeline manager. |
| `sinclude/lux/engine/render/graph/LayoutPlan.hpp` | Final binding relocation plan as compilation output. V2 derives it from generated contract/full owners; it is not an F4 physical graph execution plan. |

Existing R4 Device/VMA/Buffer/Image/ShaderModule/PipelineLayout/ComputePipeline/DescriptorPool are
extended in place only. SubmissionQueue, transfer and Retirement algorithms are byte-unchanged and
used directly by tests. F3 introduces no replacement queue, staging ring, GPU retirement authority or
background owner. Historical transfer_idle/skinning WAR failures remain historical facts, not fixed by
these native PSO tests. Final ABI/toolchain limitations are explicit in `F3_LAYOUT_CONTRACT.md` and
`F3_SHADER_CONTRACT.md`.

## Protection

Core, Transport, all historical reports, FINAL and frozen Legacy are READ-ONLY. User source checkout
and six modified files are FORBIDDEN. Root/product CMake, Runtime, Scene/ECS, Editor and Feature code
are FORBIDDEN. Additional Graph APIs and external generator changes are CONDITIONAL and require a
concrete conflict report before editing.

Preflight commands, binary diffs, byte hashes and source tree identities are archived outside source:
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F3/preflight/`.
