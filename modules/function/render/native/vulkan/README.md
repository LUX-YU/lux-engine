# Native Vulkan ownership

`InstanceContext` is owned by bootstrap, outside `DeviceSession`. A session borrows the instance and
owns one device, VMA allocator, unique queue owners and ticket-driven retirement. Aliased queue roles
resolve to the same owner. No Graph, Scene or IPC object is introduced by this foundation.

`Buffer` and `Image` own their actual Vulkan objects and release them in their destructors. Each borrows
the session's allocator, which must outlive it. Alias images share `ImageMemory`; destroying the original
image destroys its VkImage immediately without releasing memory still used by an alias. The last image
releases the VMA allocation after destroying its VkImage. Alias synchronization remains the caller's
responsibility.

`VulkanAllocator::createBuffer/createImage/aliasImage` return shared actual resource objects.
`Buffer::write/read` performs bounded mapped access and flush/invalidate; `memoryBinding()` belongs to
the resource. The allocator and device must outlive these objects, including references in queue slots.

`CommandBatch::retain()` records actual resources or one prepared binding set. On successful Submit,
the existing physical queue slot retains them until its fence advances the completed prefix. A failed
or abandoned recording releases only its own set; a later Submit failure never drops an earlier accepted
batch. Reference-array capacity is reused when slots cycle. Upload staging pages now use this path.
Object retention does not permit rewriting a page or descriptor still in use by the GPU.

`DescriptorArray::allocate()` returns a move-only `DescriptorRange`. The range keeps the array alive,
returns its generation slot on destruction, and writes only before publication. Texture backing owns
the range directly; there is no public manual slot-release operation. Retain the resource references
as well as the range: keeping a DescriptorSet alone does not keep its sampled images alive.

Buffer device addresses are explicitly enabled through `DeviceOptions::buffer_device_address`, after
checking the physical-device feature. VMA receives the matching allocation flag; creating an address
buffer without that capability returns `kUnsupported`. Any submitted BDA pointer must be accompanied
by its actual Buffer owner in the retained binding set.

The Session has no Buffer/Image identity tables or release permits. Professional snapshots and submitted
batches own the actual objects; releasing a logical resource drops its current snapshot without cancelling
accepted GPU use. There is no separate per-resource retirement admission or manual release API.
An outstanding ticket remains queue-scoped evidence of accepted work, never a frame index. Final teardown
drains queue owners before allocator/device release; individual resources never wait idle.

## Algorithm provenance

The device selection, VMA Buffer/Image RAII, bounded command/fence pool, synchronization2 submission,
queue-scoped ticket and retirement algorithms are migrated from commit
`960e755395be2c9f78a86339a062c507826c4eb4`, `modules/function/render/vulkan/`:

- `device/Device`, `device/Vma`, `memory/Memory`: actual resource factories and reverse RAII cleanup.
- `transfer/Submission`: accepted-submission serial, fence completion, cross-queue timeline waits.
- The former per-resource `retirement/Retirement` table is removed. The existing SubmissionQueue's
  completed-prefix FIFO releases each accepted batch's retained resources.
- `pinclude/Native.hpp`: test-only compile-time fault hook; no production indirect call.

The new path is `render/native/vulkan`; the old modules are not linked, and Legacy is read-only.
The old `DeviceCaps` bridge was removed and `VulkanInstance` became `InstanceContext`.
`DeviceSession` consolidates real aliased queues instead of adding another queue system.

BOUNDARY checks cover cold factory arguments and external ticket/owner inputs. STATE-CHANGE checks
cover all native results and fence completion. Successful construction establishes the internal INVARIANT
of stable device/allocator/queue borrows; no per-frame capability discovery is required.

The foundation test executes real GPU buffer fill/readback and cross-queue submission under Validation
and SyncValidation. The instrumented build exercises native initialization failures, partial admission,
retirement delayed completion and exact owner destruction. Later graph execution has its own qualification;
these foundation tests do not imply a compiled Graph or product PASS.

## G7 cold compiler

`compileGraph` consumes Definition, its successful Analysis, GraphStorageBindings, PassPrograms and Target. The analysis supplies execution
order; G7 does not call `analyze` or reconstruct dependency/version logic. The single G4 use gains an
orthogonal `EGraphUse`; Builder checks role/access legality. Unspecified usage remains valid for purely
logical G4 graphs and is rejected by Native execution, preserving historical authoring behavior.

The compiler currently supports full-range RGBA8 single-sample images, color draw/clear, transfer buffer
fill and image/buffer readback. History, compute, partial resource ranges and other Shader ABIs return
explicit unsupported errors. The cooked test shaders are real Vulkan 1.3 SPIR-V generated by glslc and
validated by spirv-val; their source and binary hashes are in the generated header. Compiler validation
uses SPIRV-Tools and the C API of the same SPIRV-Cross backend used by the existing shader toolchain.
It requires the actual reflected entry point, stage, capabilities, output and push-constant ABI. It does
not introduce another shader parser, asset catalog or MaterialGraph. Shader bytes are copied as exact
identity once; no hash-only reuse is allowed.

The single move-only `CompiledGraph` owns Pipeline/Layout, immutable recording recipes and NativeResourcePlan.
It retains no author graph or callback. Resource names and storage compatibility are cold inputs;
the later executor will use compiled indices. Compiler Buffer/Image factory counters remain zero.
Image-to-buffer output plans explicitly mark target-sized backing for compatible resize; format,
sample count and kind changes require a new compiled candidate.

## G9 preparation and execution

DeviceSession currently owns stable ExecutionContexts. Each Context prepares one physical binding set
containing its immutable CompiledGraph, allocator-owned transient images/readback buffers, image views,
descriptor pool and shared descriptor-array owner. Its CPU recipes borrow stable pointers from that set;
a successful Execute retains the whole set once in the physical submission slot. Context Storage references
keep their actual instances alive for future uses. Scope keys derive from validated Scene/View SlotKeys; attachment-private
backing additionally uses the stable context identity. Global/Scene/View sharing compares full semantic
key, storage type/ABI, size and usage during preparation only. A small typed CPU BufferInitializer is
the actual cold consumer; it does not promise a future universal GPU Storage API.

Preparation resolves all recipes and barrier handles. Execution records the immutable recipes and
checks only changing queue capacity/completion/submission results. Reusing an overwritten physical
image discards old contents but still orders its previous accesses; the first-use barrier explicitly
carries that inter-execution dependency. Unsupported graph semantics remain compiler errors.

Release destroys the CPU Context immediately, cancels Readback subscriptions and releases its shared Storage
references. It neither reserves nor appends a Context retirement record. Submitted batches retain the graph
physical binding set until actual fence completion. TextureData directly owns Image, View, Sampler and
DescriptorRange. Publication creates an immutable prepared snapshot; accepted batches keep the old snapshot through completion. Material pins keep actual
TextureData without keeping the logical Texture Storage. MeshData and MaterialData own actual Buffers;
the latter also retains its immutable TextureData pins. Instance banks own mapped Buffers and keep their
actual LastUse write-hazard checks. Submitted prepared snapshots retain these physical contents, including
indirect draw data, independently of logical Storage lifetime. None of these Storage contents reserves a
per-resource retirement record. There is no parallel ResourceDomain owner.
Shutdown drains queues and destroys contexts and scoped backing. Queue teardown releases any terminal
Device Lost batch references before allocator/device destruction.
Fault tests hold completion observation after a real submit to prove failed later submits and IPC stop
cannot destroy accepted work early. No production failure hook or additional queue is introduced.

The graphics/transfer tests execute the production compiler and contexts, verify actual readback pixels,
scope handle sharing/isolation, detach and compatible resize. Stable CPU record/execute timing includes
poll/record/submit; explicit fence wait is outside the timer. Engine C++ allocation instrumentation does
not measure driver internals. GPU timestamp duration is NOT_MEASURED.

## Convergence R5 static recording plan

Cooked SpirvArtifact belongs to data/Shader.hpp and PassProgram to graph/Program.hpp, both in lux::render.
FeatureDescriptor has no SPIR-V dependency; neither neutral header needs the Vulkan SDK.
Each compiled graph compares complete stage bytes and ABI under its single fixed layout/render state, retaining
one Pipeline owner per distinct identity. Pass recipes borrow those handles. The final G4 execution order is
unchanged. A cold plan marks necessary Pipeline binds and scope starts/ends; recorder consumes these bits directly.

Merging is intentionally limited to adjacent full-range single-color draws on the same image, with LOAD on the
second draw. This merged-scope subset has no depth, blend or conditional execution; separate Texture/Buffer tests cover
descriptors and sampling without extending these merge rules. Their color
attachment writes obey framebuffer rasterization order inside one rendering scope. CLEAR, another attachment,
intervening transfer/resource dependencies and unsupported conditions cannot be merged. No barrier is crossed or
Pass reordered. The initial physical image reuse barrier and final readback/host dependencies are preserved.

The instrumented build counts actual vkCmdBindPipeline, Begin/EndRendering, PipelineBarrier2 and Draw calls after
executing them; production expands directly to the Vulkan call. Eight GPU cases cover same/different Pipeline and
LOAD/CLEAR/other attachment/intervening transfer, plus conditional rejection and exact unique Pipeline destruction.
Draw count is unchanged. Descriptor bindings are implemented for the tested Texture/Buffer subset. Mesh batching and
Compute/Indirect/RT remain outside this implementation.

## Current component boundary

render_native_vulkan contains Device/Memory/Submission/Transfer/GpuWork/Compiler/Execution. It has no Feature include or reverse link to extensions. render_native_vulkan_extensions contains the real Texture Storage/Command implementations and GPU Command completion adapter. DirectGraph tests and the installed base-only consumer construct a G4 graph and execute real GPU readback without FeatureAssembly.

Factory installation is explicit and cold. Session contains no default Texture factory. Transfer retains
actual destination Buffers and source/destination Images in every accepted Copy/Acquire batch;
Texture keeps the source TextureData and its real synchronization
requirements for Clone. These requirements no longer control Texture destruction. Accepted-submit merging
and queue callbacks are unchanged. StagingArena owns mapped buffers and an O(1) free-list. An acquired
StagingPage is retained by CPU preparation and accepted Copy batches; only its final reference returns
the page for reuse. Session releases queued batches before the arena, including on Device Lost.
SessionUploads still contains transfer scheduling/statistics until the UploadContext migration; upload
publication currently still waits for completion observation. Page retention alone does not implement
the later submission-accepted success contract.

Mesh and Material publication visits only that resource's prepared subscriptions. Subscription removal
uses its stored position; logical deletion disconnects subscriptions while their Storage still exists.
A later binding destructor does not call a dead Storage. Buffer, Image and descriptor lifetime comes
from the submitted physical snapshot, not from keeping that logical subscription alive.

## Storage identity and lifetime

The actual Global/Scene/View/Attachment StorageSet owns each created professional instance.
`getOrCreate()` is idempotent for a compatible type/name/configuration and returns its StorageId;
it does not add a preload count. `ResourceRegistry::share()` retains an accepted internal use through
ordinary shared ownership. The registry itself only borrows the instance and its Set position.
`StorageSet::remove()` and `clear()` revoke IDs before dropping the Set references. New requests fail
on revoked IDs; existing Context subscriptions and asynchronous operations keep their instances alive.
Before publishing an asynchronous upload result, the professional Storage checks that its ID is still
registered. A removed Scene can therefore finish accepted GPU work without publishing a stale handle.
The matching Native deleter finishes before its retained configuration is released.

Name lookup uses ordered indices; ID removal repairs the dense position and erases the saved name
iterator. Clearing a Set revokes all its own IDs once, then releases the instances, including indirect
Storage references. It neither scans unrelated Sets nor waits for lease counts. GPU batches retain
physical snapshots, independently of these logical Storage references.
