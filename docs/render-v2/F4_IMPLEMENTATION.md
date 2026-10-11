# F4 Native Graph implementation and qualification contract

Entry: `25bd4e16358bfe3a8c1163a18b17b807bd6a5505`. This document describes implementation, not independent PASS.
The immutable qualification receipt will be `F4_VERIFICATION.md`. F5 is not authorized.

## Actual type responsibility inventory

Paths below are under `modules/function/render/vulkan`.

| Type / file | Value or unique ownership; borrow lifetime | Hot use and proof |
|---|---|---|
| `DispatchCommand`, `DrawCommand`, `CopyCommand`, `VNativeCommand`, `NativePassCommand` / graph/Executable.hpp | Closed immutable command values, pass ID and chosen native queue; no owner | Direct dispatch/draw or captured copy ranges; command-kind/dispatch-limit negatives |
| `NativeResourceState`, `NativeRangeState` | Vulkan state facts for exact logical range; optional R4 ticket borrow | Import validation and Sync2; family/foreign/coverage negatives |
| `NativeViewRequirement`, `NativeImportBinding`, `VNativeImport` | Synchronous borrow of caller-owned Buffer/Image/views and state spans; **not** a new allocation owner | Imported/history backing changes; caller retains backing and views through completion |
| `NativeCompileInputs` | Synchronous named borrows of F2 result, F3 initial values, R4 owners, commands/imports/sampler bank | Cold only; spans are copied or consumed, never retained |
| `NativeGraphStatistics` | Allocation/submission counts and cold timing values; allocated bytes are VMA allocation bytes, not whole-process VRAM | Alias/no-alias and cold benchmark |
| `GraphSubmission` | Existing SubmissionTicket plus slot; not a second serial authority | Stale/foreign receipt rejection, completion/CPU-read boundary |
| `NativeTraceEvent`, `ENativeTrace` | Caller-owned diagnostic record; strings only in explicit cold JSON serialization | Fixed-capacity optional trace, no hot heap |
| `ExecutableGraphPlan` | **Single** move-only composite owner of private backing; borrows device/allocator/queues | Full candidate or error; destructor preserves outstanding GPU borrows |
| `VResourceBacking` / GraphNative.hpp | Closed variant: owned R4 Buffer/Image, external reference, or unused resource | No duplicate native owner; reverse destruction preserves alias donor |
| `ResourceCell` | Atomic buffer interval / image aspect+mip+layer from logical uses | Native state only; no new F2 versions or scheduling |
| `UseRecipe`, `CopyRecipe`, `VCopyRegions`, `NativePassRecipe` | Owning immutable native masks, cell indices, copy regions, predecessor indices | No schema/name/Shader catalog access during submit |
| `AttachmentRecipe` | Field/view indices and native rendering description | Dynamic clear copied from current invocation |
| `SlotPass`, nested `ViewPatch`/`DescriptorPatch` | Own R4 ImageViews, F3 descriptor sets and bounded vectors; source-pass indices for complete shared owners | Rewrite only after prior ticket completion; full shared shape fixture |
| `GraphSlot` | Per-FIF native backing and scratch, real queue/cell completion facts | 2/3 FIF; no frame-age approximation; descriptors/views die before backing |
| `NativeGraphBacking` | Private storage of the one public owner; owns F2 copy, F3 candidates, slots and recipes | Teardown waits accepted queue tickets, including partial-submit paths |
| `EBarrierPart` / Record.cpp | Private closed value selecting full/release/acquire mask halves | Identical layouts/ranges across ownership pair |
| `CopyBlock` / Copy.cpp | Cold native packing width/bytes from existing texture format identity | No new format enum/authority; byte/alignment/range rejection |
| `EQueueRole`, `QueueLocation`, `NativeQueue` / Device.hpp | Role and actual family/index/handle facts, borrowed from VulkanDevice | Real same-family/different-family queues and fallback share handles |
| `ImageDescription`, `ImageMemoryBinding` / Memory.hpp | Single native image shape and actual VMA binding receipt | Multi mip/layer/sample owners and proved physical alias |
| `SubmissionWait` / Submission.hpp | Borrowed existing ticket + validated stage mask | Bounded three-owner wait closure, no new transport |
| `BoundDescriptorSets::{BufferWrite,ImageWrite,SamplerWrite,VWriteShape,WriteRecipe}` | Private numeric validation/write recipes in the existing descriptor owner | All writes validated first; busy/foreign/shape negatives |
| `RetirementQueue::Composite` | Private type-erased destruction of an already allocated unique owner | No per-retire allocation; capacity rejection keeps caller's unique_ptr |

Test-only support uses existing test helpers, allocation interception and the R4 private native-call seam.
`TimestampQueries` is a test-only query-pool owner. No production compiler/context/manager class was added.

## Actual algorithms and provenance

Frozen references retain SHA `a669409a289a6fa4092f21176397795b1cdb7f3e`; their relocated paths start
`render_legacy/modules/function/render/vulkan/`. They are never compile/include/codegen inputs.

| Algorithm | Active implementation | Provenance / preserved invariant |
|---|---|---|
| C9 candidate identity | `src/graph/Compile.cpp` | F3 complete NativeProgramIdentity equality, exact VkDevice and F2 cache identity; no logical-match-only native cache |
| C10 resource mapping | `Resources.cpp`, existing Memory/ImageBindings | R4 VMA RAII; V1 `src/gpu/memory/` remains reference, no DeviceContext hierarchy |
| Conservative alias | `orderedAlias`, existing `Image::alias` | Actual same VkQueue execution order, disjoint uses, no conditional/export/history, exact shape/usage and memory requirements; private per-FIF donors. V1 transient lifetimes alone are not GPU proof |
| C11 full owner binding | `Recipes.cpp`, F3 BoundDescriptorSets | F3 complete OwnerShape remains authoritative; missing/ambiguous shared declaration fails; cold numeric source-pass recipes |
| Copy/staging/readback | `Copy.cpp`, R4 StagingArena/SubmissionQueue | V1 `src/gpu/transfer/TransferScheduler.cpp` and R4 stage/pin/complete invariant; no copied scheduler/worker/interface hierarchy |
| C12 Sync2 | `Sync.cpp`, `Record.cpp` | F2 order/versions/choices unchanged; V1 `src/graph/RenderGraphCompiler.Barriers.cpp` and `pinclude/lux/engine/render/graph/RGBarrierUtils.hpp` inform native scope/layout mapping |
| Queue ownership | R4 SubmissionQueue plus native recipes | Release on actual prior queue, semaphore wait, acquire on destination; same-family distinct queues still wait. No family transfer when family equal |
| C13 candidate and retirement | RAII compile local result, `Export.cpp`, R4 RetirementQueue | V1 `pinclude/lux/engine/render/renderer/FrameRetirementPlan.hpp` last-use invariant; no frame-number heuristic or routine DeviceWaitIdle |

Vulkan synchronization reference: [official examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html).
A same-family layout transition is chained after the semaphore wait's destination stage; setting its source to NONE
was caught by development SyncValidation and fixed. Different-family waits cover the acquire operation with ALL_COMMANDS
because maintenance8 is not enabled; resource stage/access masks stay precise. Final completion joins also use ALL_COMMANDS.
These are explicit completion/ownership scopes, not a blanket resource-barrier substitute.

Native local reads use [dynamic-rendering-local-read](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering_local_read.html):
read-only descriptor attachments and F2-approved same-attachment pairs use GENERAL with combined framebuffer masks.
The paired fixture is one fullscreen triangle, no overlapping-fragment ordering claim. F4 does not enable rasterization-order
attachment access or implement an OIT algorithm. Disabled native capability uses a separately authored sampled-input pass.

## Ownership and execution boundary

Device/allocator/three queue owners outlive ExecutableGraphPlan and imported resources. The plan owns F3 candidates and
all per-slot R4 native objects; F3 pools/sets borrow those layouts and resources. Slots release descriptors and views first,
then backing in reverse declaration order, then F3 pipeline/layout owners. Alias images own only VkImage; donor owns allocation.
Accepted submissions pin the entire slot. The final graphics-domain ticket joins all queue last uses; it is not a frame serial.
A partly submitted failure is terminal and retains every accepted ticket for teardown; it cannot expose a fictitious lastUse proof.

Imported backing/views stay external. Input initial state is a caller promise; real tickets prove readiness, not the truth
of arbitrary external recorder barriers. Range states must cover each native cell exactly once. Queue family is derived from
an admitted ticket when omitted; an explicit mismatching family is rejected. An in-flight backing reused by another slot
requires its actual last-use ticket. `resourceStates` returns precise final states/tickets; `exportedBacking` borrows only
explicit outputs; external GPU users return their last ticket through `retainExternalUse` before slot reuse or retirement.

No automatic retries, threads, frame loop, Scene/View owner, Present or swapchain is introduced. CPU access to a plan and
its queues is externally serialized, consistent with R4 queue ownership. Completion polling never means implicit image visibility.
Readback is available only for explicit READBACK output buffers or a HOST_READBACK recipe's private buffer after completion.

Current native recipes cover absolute 2D (including arrays/mips/samples), direct procedural graphics, compute and copy/readback.
Unsupported 3D/cube/relative extents and vertex-stream graphics return structured errors; they are **not device capability
fallbacks** and no geometry/volume business capability is claimed. F8 supplies actual mesh/indirect consumers; F5 supplies
actual target/presentation ownership. Copy depth/stencil uses a graphics-capable queue unless its newer transfer capabilities
are explicitly enabled in a future concrete consumer.

## Test mapping (results belong to independent V)

| Test | Required evidence |
|---|---|
| `render.vulkan.native_graph` | W02 one image/4 mips/2 layers HZB oracle; W03 single/three families/three same-family queues; buffer-buffer-image-image-host with two mips and dynamic offsets; export to second graph and safe retirement |
| same executable, `NativeGraphReadWrite.cpp` | Intermediate versions 0/1/2 read back around READ_WRITE passes; RAW/WAR/WAW and repeated FIF |
| same executable, raster | MSAA4 resolve; depth/stencil LOAD/CLEAR/STORE; red remains after rejected green; later accepted blue |
| same executable, local/conditional/import/shared | Native input attachment and same-attachment read vs explicit fallback oracle; conditional skips choose current F2 fallback; prepared imported-view changes/history epochs; complete shared Scene/Feature sets |
| same executable, views/alias | W16 native layered vs separate output equality; W04 actual VkDeviceMemory+offset alias evidence, independent FIF backing; overlap/multi-queue/incompatible shape conservatively no-alias |
| `render.vulkan.graph_candidate` | 12 candidate fault points including second-slot rollback, last-good retained, busy/foreign/shape descriptor failures, held completion cannot recycle, bounded retirement, injected VkResult preserved, zero native owner creation during submit |
| `render.vulkan.graph_benchmark` | Warmed actual 256x256 compute/graphics/copy: C++ heap count/bytes, raw p50/p95/max; GPU interval includes CPU submission gaps; cold compile phases and total with prebuilt PSOs |
| public header and six native reject probes | C++20 public closure, no Scene/Runtime/Transport/Legacy/Editor/Feature input |
| old 137 obligations | Preserved; actual final normal/full-ASan integration, F3 hot consumer map excludes reflection/relocation |

Trace records pass identity, range, before/after masks/layout/family, selected resource and skip; cold JSON links resources
with F2 versions/writers/readers and actual memory bindings. No hot string construction. Per-copy private host visibility
barriers are fixed recipes in Copy.cpp; these private readback buffers have no semantic GraphResourceId.

## Qualification limitations and future gates

Independent results must distinguish real native success, injected failure, unavailable platform, and unimplemented future
responsibilities. Device Lost is injected after real outstanding work completes; no physical device loss is claimed.
No Linux, Android, installed-SDK consumer, product, Runtime/pacing or business Feature qualification is claimed here.
Historical transfer_idle and skinning WAR remain historical findings; F4 tests cannot rewrite their old verdicts.
V2_PRODUCT=EXPECTED_UNAVAILABLE; F5=NOT_AUTHORIZED; F6/F8/F11 remain future stage obligations.
