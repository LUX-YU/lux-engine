# Render

Render is one shared library. Applications link `lux::engine::function::render` from
`find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render)`. Loading the library does not create a GPU
device. Backend resources and implementation dependencies stay inside the library.

The external [LUX-YU/NRI](https://github.com/LUX-YU/NRI) fork is based on v192. Its exact commit is pinned in
`bootstrap/lux-manifest.json`; the default checkout is the sibling `NRI` repository, overridable with
`LUX_NRI_SOURCE_DIR`. No NRI source or configure-time patch is carried in lux-engine.
Windows builds include Vulkan and D3D12; `DeviceOptions::backend` selects the backend
(Vulkan by default). NRI owns the native device and physical
allocations; Render owns its submission, upload and frame policies. Installed extension headers expose the pinned
NRI declarations explicitly. Vulkan and VMA implementation dependencies remain private. Checked backend failures
remain structured Render errors.

## Public and project contracts

- `include/` contains installed values, Commands and Server APIs. `include/.../extensions` contains graph,
  Storage, Handler, device and upload author contracts; the implementation is still compiled only once.
- `sinclude/` contains uninstalled graph compiler, execution-instance and built-in composition contracts.
- `pinclude/` contains concrete built-in Storage/Handler implementations and private algorithms.
- `src/` follows the same responsibilities. A single target compiles each production source once.

Neutral Command and resource value headers require neither NRI nor stdexec. Asynchronous Handler contracts use
stdexec; GPU extension contracts use the NRI declarations installed with this SDK. Extensions compile against the
same selected backend and C++ ABI and use the existing device through these contracts, without linking a second NRI.

Extension authors select `render_extensions` in `find_package(lux-engine-render ...)` and link
`lux::engine::function::render_extensions`. This INTERFACE target adds author dependencies to the existing Render
library; it produces no second DLL. NRI and execution-template includes do not propagate from the ordinary Render
target. An application that creates ExecutionRuntime also declares its own Core Execution dependency.

Server creation registers built-in construction/call entries. A public capability is a static type declaring
`PrerequisiteCapabilities`, `Resources`, and Effect `Commands`. `CapabilityDescription` stores their borrowed
metadata. Resource declarations alone own their commands and per-frame element types; Capability implementations
cannot redirect those calls or substitute another Resource identity. Composition deduplicates Resource requirements
across selected capabilities. `ListCapabilities` discovers concrete implementations by capability type name and implementation name;
results contain the public declaration, configuration qualification and logical final output names, not resource schemas.
The saved composition keeps names and actual configuration bytes; generational IDs belong only to this Server.

Resource upload is independent of Effect creation. `GetOrCreateResource` selects an instance in a real
domain, and create/upload commands carry its resource_id. Item commands carry complete parent/local IDs.
`CreateEffect` selects the graph composition and output contract. FrameData identifies a Resource declaration
and element type; Attachment resolves the graph's declared Resource requirements.

Views share an immutable logical Effect and the device program cache. Each execution owns its native Plan
and graph instance. Mesh/material IDs, grouping, shadow
layers and UI images are live instance/frame data, not Effect compilation inputs. Late material programs prepare
on the shared CPU scheduler and are adopted on RenderThread before recording a frame. A failed frame preserves
the previous program cache but does not promise to roll back already adopted resource inputs; its completion
reports the required resynchronization. Only DeviceSession's cleanup scope retires accepted physical uses.

One dynamic Raster pass consumes zero or more prepared Draws/Pipelines. Its per-FIF arrays, descriptors and
parameters belong to the instance. Actual external reads include sampled Target images, material texture tables
and BDA-addressed buffers; binding changes rebuild the combined submission dependencies before recording.

Resource definitions declare their Commands. Each Resource registration supplies the corresponding static typed thunks;
CommandRegistry owns those calls and ResourceRegistry records their locations by declaration and CommandId.
Capability `Bindings` contains only independent capability methods. Mesh upload consumes `rdesc::Mesh`; clients do not produce Native GPU layouts.
Handlers use the existing professional Storage Senders and ResourceUploader. Transform and light elements use Resource-declared PerFrameData in one frame batch. Camera uses a View command;
UI uses an explicit borrowed-array submission command. Resource inputs never generate per-item Requests or Responses.

An implementation declares `using Capability = ...`, `Features`, `Storages`, `Bindings`, and its
configuration type. `registerCapabilityImplementation<I>()` checks the external declaration before publishing factories
and exact methods. Registry membership owns the capability-to-implementation relation; implementation records do not
repeat CapabilityId or CapabilityDescription. Cold add/replace/remove calls reject active execution and invalid entries;
replacement creates a fresh generation. Callback/type metadata must outlive the Server. No plugin lifetime service is added.

One capability can use several graph Features. The built-in Lighting3d implementation connects shadow projection,
shadow rasterization, Clustered Lighting, deferred lighting and Tonemap inside one EffectDefinition. These algorithms
are not separately advertised capabilities. `DeferredLightingConfiguration` owns their actual switches/settings;
`ClusterGrid`, shadow settings and color encoding are data, not additional configuration services.

`CreateEffect` lowers all selected contributions to the existing Graph/Analysis without receiving a native Device,
Core interface or queues. Attach/replacement currently provides the execution-candidate boundary: Compile and native
program preparation finish before the candidate is adopted. Typed cold values are
owned once and referenced by index. Resource port semantics, shapes and compatibility remain in the extension-only
`CapabilityGraph.hpp` compiler interface. The ordinary directory does not publish that graph metadata. Engine recipes
collect data by the static capability type, not by inspecting internal ports or selecting a payload schema.

Command/configuration/binding identity uses `cxx::type_info` from `make_basic_type_info`. It is C++ type agreement for
compatible builds, not cross-compiler serialization. Actual file codecs preserve their own format identity. Type metadata,
callbacks and destructors must remain loaded until every dependent operation/value has been cleared.

Ordinary Features declare `Config`, `Ports::describe` and `build`; registration generates their erased
`FeatureGraphOps` thunks. `Ports::describe` exposes config-dependent port information without building a graph or
creating device objects. Composition calls that same cold entry. No Feature object or empty runtime hook is required. Imported ports carry their initial use; selected exports
carry their exact range and final use. Concrete graph building remains in the project-level Feature implementation.

## Requests and execution

Framework commands are split by Scene, View, Target, Effect, Discovery and Server responsibilities.
`commands/Commands.hpp` is their aggregate; the remaining Attachment lifecycle protocol is separate.
Mesh and Instance commands have independent namespaces under `resources/mesh` and `resources/instance`.

Server catalog setup belongs to the thread that created the Server. The host completes setup before starting
`run()` and joins it before post-run inspection or destruction. During `run()`, catalog access belongs to its
actual owner stack; unrelated threads are rejected before any mutable directory read. Handler and stateless
command installation additionally require no active frame or structural command. Runtime clients discover IDs
through `QueryCommandIds` (fixed bootstrap ID); `RenderClient::create` uses that IPC path. Names are cold lookup
keys; accepted commands use IDs and the declaration-specific binding from one Resource qualification.

Use `registerCommands` with `CommandBinding::declare<C, function>()` for stateless typed sync/async functions.
Capability `Bindings` also accepts such functions, selecting their exact implementation without a dummy owner.
Stateful Handlers retain constructor-injected dependencies. All forms use the existing request operation,
Resource borrow and ResponseOutbox; an async request's source bytes remain borrowed until its real response.

An Attachment owns its professional Feature bindings and Storage subscriptions. The graph instance receives borrowed
`ResourceInput` values; it does not discover Storage, own Feature objects or call their preparation functions.
FrameExecution prepares the Attachment's borrowed work at the reusable FIF slot. ResourceUploader records its
preparation copies before FrameExecutor records graph work. Detach and replacement disconnect the professional
objects and subscriptions before destroying the graph instance; physical cleanup still uses the Session scope.

Image/Buffer copies distinguish capacity from initialized content. With fixed dimensions and an explicit row pitch,
only actual row bytes become defined; padding remains undefined. A destination `READ_WRITE` explicitly preserves
an already defined range, including padding. It is required when dimensions or multi-row pitch are resolved only at
instantiation. Count and indirect argument accesses retain their actual offsets and byte ranges even though NRI
Buffer barriers operate on the whole physical buffer.

Graph/plan JSON and DOT describe dependencies, resource ranges, layouts, rendering attachments and barrier provenance.
`FrameExecution::dumpSubmissions` separately reports the latest accepted batch per FIF slot and current frame queue
frontiers. These are bounded submission facts, not GPU timing or a continuous trace.

Applications use Platform IPC directly. They discover CommandId, attach it with `commandTag()`, send a typed
Request and consume the corresponding Response. Responses contain `RenderError error` and business fields;
zero `error.type` means success. Internal fallible functions use `RenderResult<T>` where an error is recoverable.
The library returns structured diagnostics and never chooses a terminal or logging destination.

Before `run()`, the application selects one IPC Link with `setFrameSource()`. That Client sends a sealed
`SubmitFrame`: a RenderFrame borrowing one sealed FrameData and naming its existing Attachment execution.
`QueryFrameInputLayout` reads that execution's already assembled schema; it creates neither Resources nor execution.
Admission checks the exact layout/revision and mutable instance identities before any apply. Each Resource's immutable
record holds its actual target, callback and column indices; one synchronous apply receives all of its element spans.
There is no phase protocol, capability ordinal destination, payload codec or external input clock.
After all started inputs settle the Server prepares/submits the frame and publishes one root FrameCompletion.
An empty packet explicitly renders unchanged state; an empty IPC queue does not generate frames.

The Client reserves one of `max_frames_ahead + 1` stable input carriers before preparing a game frame.
`FrameCompletion` ends the input borrow and describes accepted rendering submissions, not GPU or presentation
completion. Physical FIF slices are reused only after all their real Queue frontiers complete. A failed frame
can have accepted updates; the Client must provide an explicit resynchronization frame before normal frames
resume. Asset Clients do not bypass this visible-state boundary, and disconnect does not end source borrows.

Synchronous Handlers fill the Response directly in the Outbox message. Asynchronous Handlers return a concrete
Sender. `counting_scope + spawn` owns connected operation states. Errors and stopped completions become typed
Responses, and `continues_on` delivers them to the RenderThread's `ResponseOutbox`. The schedule operation
guarantees this final handoff even during shutdown, preserving failures and unknown outcomes. No separate
result record or hand-written completion callback owns the Response. The RenderThread publishes IPC replies.
Each asynchronous Command, including SubmitFrame, still has stdexec's operation-state allocation. Allocation-free
steady recording is a narrower claim than allocation-free Client-to-Server frame submission.
The service loop adopts ready CPU results before dispatch, runs continuations made ready by GPU completion/write
admission before frame preparation, then delivers terminals produced by the frame. These three explicit safe points
also remain active while the Command Scope drains. GPU polling itself does not drain the Server ManualContext. The Context uses delivery scheduling and remains
open until accepted operations, publisher tails and required cleanup have settled.
Outbox service counts settled records, including replies discarded for closed destinations; it is not a delivery
acknowledgement. The wakeable idle revisit remains bounded because one IPC receive examines only part of the Links.
The CommandRegistry owns one Command directory, stable Handler objects and one selected binding index per
Command. External installation validates its entries before publishing. Framework handler selection affects future framework dispatch. Capability requests use their exact bound method;
an operation already started never changes its Handler.

Upload sources are borrowed. After successful submission, the Client keeps their addresses, extents and contents
valid until Response, including failure or cancellation. Failed submission takes no borrow. Abandon or connection
loss does not prove reading ended; the application settles the Server and CPU producers before releasing such
inputs. Native staging and submitted GPU resources remain the device's responsibility.

`ConfigValue` exclusively owns one immutable typed configuration. Its defining code must remain loaded through
destruction. The compile request owns configurations during asynchronous compilation; graph assembly borrows them
synchronously. Dynamic configuration cost counts actual owned capacity, not external upload bytes.

## Graph and Features

Logical `GraphPass::work` is an immutable type-erased `PassWork` recipe, not a closed Raster/Compute/Transfer variant.
The existing convenience constructors normalize their ordinary work into that same graph owner. Custom authors use
`PassWork::create<Feature>(config, policy)` and `RenderGraphBuilder::addEncodedPass` with an explicit PassDeclaration:
resource versions/ranges/usages, a conservative native stage union, queue kind, optional common shader interface, and
ordinary GraphPassOptions for side effects/conditions. The common Builder still validates and versions all accesses;
analysis and Feature ownership checks see the same graph metadata. Empty undeclared side effects are rejected.

A typed cold `Feature::compile(config, PassCompileContext&) noexcept` returns immutable encoding state.
`PassEncoding` requires a const/noexcept record returning RenderResult<void>. Its RecordInput contains resolved frozen
resources/parameters and caller-exclusive scratch, with no Server/Registry/scheduler/GraphInstance lookup. Record only
encodes into the supplied exclusive command buffer: no Submit, GPU wait, shader compilation, registry changes or unbounded
allocation. Extension code must outlive every recipe, compiled state, thunk invocation and deleter.

MANAGED encodings provide the operation body; common raster rendering scope, pipeline/descriptors and dynamic state stay
with the executor. EXCLUSIVE takeover owns its rendering scope and always invalidates the supplied binding cache, including
on record failure. Merely sharing immutable logical recipes does not share mutable recording scratch or native state.

Both convenience frontends and custom recipes lower to the same `CompiledPass::work` (`PassEncoding`).
The compiler retains the ordinary resource, layout, rendering-group and submission planning algorithms; the
executor has no central Raster/Compute/Transfer command variant or type visitor. Ordinary raster groups use the
common managed rendering scope. Dynamic draw lists explicitly own their sections with EXCLUSIVE encoding.
Compilation receives target-selected programs and the planned native descriptor layout. The common path binds
MANAGED layouts, descriptors and constants; EXCLUSIVE code binds its own state. Custom shader code must use the
provided target and physical layout. Record failure ends the command buffer and prevents its submission.
Custom Raster declares `PassDeclaration::rendering` using the same `RasterScope` as ordinary Raster. Attachment
versions, mip/layer views, depth and resolve targets enter the common Builder/Compile/Instance path. MANAGED graphics
requires a scope; EXCLUSIVE records its own Begin/End using the prepared native views. `PassCompileContext::rendering`
provides target format/sample facts without passing physical images into logical construction. Attachment/copy barriers
use fixed-function stages; custom shader-stage unions are filtered to compatible shader accesses.

`PassEncodingPolicy::scratch_bytes` reserves per-pass storage in each FIF slice before recording. Ordinary compiled
states are stored inline; extension states larger than that inline capacity own a separate allocation. A Plan retires
encoding-owned GPU objects at the DeviceSession's accepted submission frontiers, then releases their layout leases.
Prepared `PassDescriptorBindings` contain only native layout and set locations, not the compiler's reflection tables.

`RenderGraphBuilder` owns candidate arrays. `finish()` produces one self-contained immutable Definition;
`analyzeRenderGraph()` produces independent analysis data. Graph resource/pass IDs are indices within that Definition, not
global generational identities. Version zero describes explicit initial contents; positive versions name writes.
Reads, read/write predecessors, conditional fallbacks and overlapping ranges are explicit. Exports and side effects
root culling. Invalid ranges, live cycles, ambiguous producers and incomplete reads fail with structured errors.
Untouched subranges inherit the explicit predecessor; analysis follows those ranges only when a consumer observes
them. Logical lifetime disjointness does not authorize physical memory aliasing.

`addRasterPass`, `addComputePass` and `addTransferPass` derive accesses from the work's actual inputs,
attachments and operations. They return the new written versions; authors do not repeat the same accesses
in a separate Program callback. Raster attachment load/store operations are explicit. A partial texture copy
preserves its predecessor; it cannot manufacture initialized contents outside the copied rectangle.

`EffectDefinition` selects Feature contributions and connects their named ports explicitly. Identical port names
do not connect implicitly. `composeFeatures` sorts ready contributions by full type/contribution key, invokes each
build once, audits its appended resource/pass interval and analyzes the finished graph once. A consumer may read
the supplied version, while only its producer may write that output. Output versions are not restricted to v1.
Zero-pass resource Features need no dummy build callback.

`FeatureEntry` retains the selected cold build callback and one capability declaration; private tuples carry
implementation dependencies. Ordinary clients select capabilities and semantic payloads. Extension authors use
`render_extensions`; built-in raster implementations and dependency lists remain private. Features describe
draws, compilation lowers them, and execution records them. No Feature build callback or graph analysis runs in a
stable frame. Cooked shader reflection remains the source of parameter layouts.

`GraphCompiler::compile` consumes the immutable graph and its analysis, without a Storage registry or Feature
factory. `ShaderBindingPlan` determines target binding placement, PipelineLayout and Descriptor writes together.
Private descriptor arrays retain their actual type/count/stages; the shared image/sampler table always uses its
complete allocation shape. Ordinary sampler inputs are separate dense graph-local input slots: supply borrowed
samplers through `RenderGraphInstance::bindSampler` before initialization. They do not create image hazards.

The Effect keeps Storage requirements and cold Feature creation data. Attach resolves those inputs and creates
professional bindings before initializing the `RenderGraphInstance`. The instance allocates graph resources and
descriptors per existing FIF slot; it neither discovers Storage nor runs factories. Standalone project GPU consumers
can bind physical inputs directly without creating a Registry or Server.

The `RenderPlan` contains typed recording work, range transitions and rendering groups. Legacy's consecutive-pass
grouping and execution-order last-use rules are reused with stricter attachment identity, range and dependency
checks. Only compatible LOAD successors share a rendering scope; sampling an earlier attachment requires the
corresponding barrier outside rendering. The NRI backend has no discard load operation, so author DISCARD is
lowered to a clear, while discard stores remain native discard stores.

`RenderGraphInstance::recordBatch` owns the exclusive per-batch/FIF allocator reset and command-buffer Begin/End.
`FrameExecutor` only encodes through the supplied NRI core and a const view of prepared instance data. Shared
descriptor handles are resolved during preparation, including Vulkan table replacement and D3D12 frame-local
snapshots. These inputs stay separate from the mutable binding cache, so invalidation cannot erase them.
Professional preparation
may contribute bounded Buffer copies; it cannot hide arbitrary GPU dispatch behind a recording callback.
ComputeWork can permit async compute. Compilation consumes immutable physical queue facts, folds aliased roles,
and groups adjacent work into SubmissionBatch intervals. Imported images without an explicit sharing contract
remain on Graphics. Explicit Buffer/Texture copies are available; there is no
production Readback service or implicit extra rendering.

The default cold allocation strategy reuses identical transient objects, then aliases compatible Memory where
the original submission dependencies and physical Queue FIFO prove all earlier uses precede all later uses.
Imported resources, History, exports, conditional resources, initial contents and opaque shared tables are excluded.
Project callers can explicitly select independent allocation or object reuse without Memory aliasing.
Attach resolves actual NRI memory requirements. Dedicated allocations, incompatible memory types and activation
inside a merged rendering scope remain separate. Aliased members share offset zero within one resource kind;
this is not a general mixed Buffer/Texture heap packer. No dependency or CPU wait is added to make reuse possible.

Each FIF slot owns its physical objects and Memory independently. Alias activation establishes a memory barrier
before the new object's layout/access transitions. Cleanup releases descriptors, then resources, then each Memory
exactly once through the existing Session cleanup scope. Plan diagnostics report logical-to-physical mappings;
instance resource diagnostics report actual Memory groups, requested bytes and placement rejection reasons.

FrameExecution advances a frozen submission window across Queue capacity limits. Each accepted batch records its
actual queue serial; dependencies wait for that producer, never an unrelated later submission. Compatible ready
intervals from multiple instances share a submit. Shared imported reads remain independent; overlapping writes
preserve instance order. NRI concurrent graph resources use semaphore memory dependencies and consumer-side
layout transitions. First and final Graphics boundaries cover preparation copies, History initialization and
canonical resource states.

Each FIF slot reuses its dependency indexes while the ordered instance set and imported physical bindings stay
unchanged. Content-only updates do not sort or intersect imported ranges. Binding replacement, range changes,
Resize or a different instance invalidate that slot's index; rebuilding still has a cold worst-case quadratic
conflict enumeration inside a shared physical-resource group. This is not a second Graph or a GraphAnalysis run.
`dumpSubmissions` reports the rebuild and comparison counts separately from accepted submissions.

Imported accesses retain their consuming batch and required canonical-state release boundary. Ready batches
are indexed per physical Queue by their actual wait conditions; accepted submission advances only their
successors. The compiled plan explicitly preserves physical-Queue FIFO used by object reuse and memory alias
proofs. Queue selection can rotate without redefining the order of two writers of one imported range.
The existing imported binding API identifies actual Buffer/Texture objects; independently supplied aliased
objects with different native handles have no declared allocation relationship and are not supported by this
cross-instance contract. Graph-owned Memory Alias retains its existing allocation and dependency proof.

Preparation-copy completion and complete-instance publication are separate. Accepted copies can settle their
upload Senders before the graph finishes submitting. History publishes only after the instance's required batches
are accepted; partial failure stops that instance until replacement. FIF reuse waits for every accepted physical
queue. Root parameters, viewport, conditions and History inputs stay fixed throughout a suspended CPU window.
Destroy/Detach/Resize of an instance still in that window returns busy. Professional physical release also returns
busy while such a window exists; it does not destroy objects referenced by future batches. Standalone callers must
finish or cancel that window before destroying its borrowed inputs. These checks end at CPU submission completion,
not GPU completion; physical cleanup still uses only DeviceSession.cleanup_scope_.

`declareHistory` creates a Previous/Current pair. `publishHistory` observes the complete resulting version,
including inherited mip ranges. Distance 1 is supported. Image initial values are explicit; Buffer history currently
starts at zero and requires a multiple-of-four byte size. The instance owns F+1 physical slots, chooses a free write
slot by actual completion frontiers, and publishes only after the instance submission is accepted. A skipped writer
does not publish a sample. Descriptor views are created at Attach, not on each rotation. Resize replaces only
History banks whose physical extent changed; other banks retain their allocation and invalidate their samples.
`InvalidateHistory` invalidates the selected attachment histories at the next preparation boundary.
An individual Previous read can name a compatible non-History resource as its first-sample fallback.
Analysis retains both inputs and their hazards; Prepare selects existing views or copy sources without copying
the fallback into a History slot. Different reads can declare different initial inputs. Fallback views are cached
with their frame bindings and change only when their physical input changes.

Resize keeps the `RenderGraphInstance`, its Storage subscriptions, Feature bindings, descriptor pools and immutable
plan. Compiled reverse indexes locate the affected views, barriers, dispatches and copy layouts. A finite
`ResizeResources` candidate owns only replacement physical objects; adoption exchanges those objects without
recreating unrelated buffers or fixed-size images. Alias migration includes the connected closure of old and new
allocation groups. Actual placement still uses `instantiatePlacements` and the existing batch-dependency proof.

The Server coordinates a Target's producer and consumers as one resource adoption. Existing Queue completion
Senders wait for their captured accepted frontiers while the RenderThread continues other completion work.
Replaced objects use the sole `DeviceSession::cleanup_scope_`. WSI close retains its separate presentation
requirement; a failed replacement after closing the old SwapChain leaves an unavailable surface, with old output
borrows withdrawn. Resize replies describe adopted resources, not rendered pixels or completed presentation. Success pauses the
Target. Publish matching inputs and `SetTargetActive{target, true, epoch}` in the next complete frame; activation
checks the replied epoch so an older queued frame cannot resume resized output. A failed reply reports whether
physical output remains available, including an unavailable SwapChain after failed replacement.

`declareCondition` declares a dense Boolean input; `PassCondition` selects that input or History validity.
Attachment conditions are zero/one bytes and are sampled once during preparation. Conditional writes require
an available predecessor fallback. Consecutive passes sharing a predicate form one non-nested ConditionalRegion
and sample it once. Both paths record the same compiled transitions; the false path preserves predecessor content.
Rendering groups may merge within a region but never across its boundary. There are no predicate callbacks or
GPU-to-CPU reads.

The private `HzbFeature` consumes forward-Z depth and produces current/previous R32 depth pyramids. It ports Legacy's
per-mip maximum reduction with complete odd-size footprint coverage. History, initialization and barriers belong to
the graph instance and plan; the feature has no private double-buffer or barrier owner. `ImageDispatchArguments`
resolves group counts from the actual attached mip extent, including after Resize.

`dumpGraph` and the project-side `dumpPlan` synchronously emit JSON or DOT into a borrowed sink.
They do not open files, wait for the GPU or trace stable frames. Compiled plan counts are distinct from measured
execution counters.

## Resources and frame lifetime

DeviceSession contains the device, immutable capability facts, unique physical Queue collection, shared
DescriptorTable and `cleanup_scope_`. The selected backend's extension table is private to its implementation.
ResourceUploader owns staging, upload batches, admitted resource write borrows and copy-acceptance callbacks.
FrameExecution owns frame-slot rotation and preparation/submission phases; it opens the Uploader's write window
only for the actual reusable slice. ResourceCreateContext contains DeviceSession, ResourceUploader, frame count,
ResourceRegistry and the addressing choice; it contains no FrameExecution pointer or erased executor callback.
Device, Scene and View each own a ResourceSet whose records directly contain StorageOwner. ResourceRegistry
keeps the sole declaration directory and a non-owning generational index. Effect owns its compiled graph. Attachment owns the execution instance and borrows the Target's output. The Client settles uploads
and detaches consumers before deleting Storage; arbitrary early deletion is not a cancellation API.

DescriptorTable maintains independent image and sampler indices. Identical sampler descriptions reuse one
device-lifetime sampler. Texture references carry both indices and explicit flags; the material sampling code
returns white for its default flag without accessing a descriptor. The device does not allocate a white texture.
Cooked material payload version 11 includes the target identity and its own layout; version 9 remains readable as
legacy SPIR-V data. Former v10 expression assets require rederivation from MaterialSource; Render consumes
only target programs, never author graphs or compiler sessions.

The default is two frames in flight, configurable from one to three. Dynamic instance fields, page tables,
material parameters, descriptor records, indirect data and Target outputs have physical frame slices. CPU business
data and immutable programs remain single authorities. A frame slot is reusable only after its CPU writers end
and every actual participating physical Queue completes.

Storage upload Senders use the existing CPU scheduler and RenderThread resumption. ResourceUploader packs finite
staging windows and batches Copy work. Frame preparation contributes missing ranges before compatible graph
recordings enter the shared submission. Dirty-word batching and contiguous Transform input avoid per-element Copy
construction. There is no per-frame Sender for unchanged drawing and no unconditional sleep in the event loop.
Within that same Task, a contiguous Buffer window of at most `TransferOptions::inline_copy_bytes` is prepared
directly on RenderThread (initial default 16 KiB). Larger copies, strided packing and image rows use the shared
CPU scheduler. The threshold is configurable, including zero to disable it; it is not a measured universal optimum.
CPU upload clock sampling is disabled unless `measure_cpu` is set. `TransferStats::cpu_timing_enabled` distinguishes
unmeasured zero counters from measurements; GPU query sampling remains independently controlled.

TransformBatch borrows separate key and matrix spans with equal lengths and unique, current keys. The Client
retains both spans through Response and supplies finite matrices. Create/Update instance arrays follow the same
borrow rule. Storage validates identities at admission and retains one GPU-format copy of adopted data to fill
older FIF slices. This is a replica source, not another authoritative scene model. A successful upload adopts
the new CPU values only after all necessary submissions are accepted. A partial failure keeps the previous
adopted values and marks affected GPU ranges for repair before another Draw. Client buffers may then be reused.
Buffer growth keeps the previously published binding until the complete candidate is accepted; failed growth
cannot expose a half-populated buffer. Repair failure stops rendering instead of drawing mixed revisions.

FIF replica preparation reserves one complete snapshot across physical staging pages. Insufficient staging or
CopyRange capacity rejects the whole preparation before submission; it does not publish a partially updated slice.
Failed preparation or submission retains dirty revisions. Large stable sources are copied on the shared CPU scheduler;
bounded transient projections use `stageInlineBuffer` to freeze their scratch bytes before the callback returns.
A fully prepared replacement waits for actual acceptance before publishing its binding. This can precede graph
recording, but never splits the replacement to compensate for insufficient staging. Descriptor publication and dirty
adoption stay on the control owner. FrameExecution owns frame command resources;
ResourceUploader owns its independent per-physical-queue upload command windows and StagingArena. Queue owns neither.
The host settles accepted GPU use before destroying either command owner; destruction checks this contract and never
hides an outstanding use with a wait.

The Runtime rebuilds its non-owning Storage preparation list when Attachment membership or RenderGraphInstance identity
changes. Project GPU consumers collect the actual `StorageFrameUse` borrows at those changes and supply their span
to `FrameExecution::render`; each entry names its storage and `StorageBindingOps`. Preparation callbacks are synchronous:
they do not submit, suspend or dispatch completion callbacks. Stable frames neither rebuild this list nor sort
Storage relationships.
Qualified entries can supply their existing ResourceInstanceId and a borrowed preparation revision. Each FIF slice then
skips an unchanged, successfully prepared source; failed preparation/Submit does not acknowledge the revision.
An entry without that explicit qualification retains per-frame preparation. Feature-derived data can likewise
provide a revision, while camera/light projection and other true per-frame inputs retain their preparation callback.
The source-list walk is still linear in active sources; skipping callbacks does not make it a new event queue.

The compiled Pass offsets select one contiguous parameter region per FIF slice. Instance/default changes mark
only their compiled consumers. `ResourceInput::parameter_revision` is zero for a live external borrow, which
continues to be read each frame. A nonzero value promises unchanged bytes until `setResource` publishes a new
revision; immutable texture references and shadow-projection outputs use that contract. Address equality alone
never proves parameter contents unchanged. Default bytes are not recopied for a mapped-input-only change.

One `BindingState` belongs to each actual CommandBuffer recording, including merged instance batches. Compatible
physical descriptor sets can be retained across Passes; a new Layout/BindPoint, clear helper or CommandBuffer
invalidates that cache. Logical vertex-resource indices remain local to an instance and are not shared by this cache.

When GPU work prevents progress, the loop first waits on the conditions preventing the next frame slot's reuse,
then rotates other in-flight physical Queues. IPC, ready work and stop are checked between waits. The default
`gpu_wait_ns` remains 1 ms; it bounds the requested GPU wait, not end-to-end Command latency. Fully idle operation
uses the existing wakeable IPC wait.

Buffers and ordinary sampled textures use the backend's concurrent sharing across discovered
queue families; frame-local attachments use exclusive sharing. Execution dependencies and access
barriers remain explicit. Texture creation checks the complete pinned Vulkan image-create key
on the cold path, including usage, samples and backend-added flags.

NRI Buffer barriers cover the whole resource. Adjacent, ordered frame copies to disjoint ranges of one Buffer
share a transition pair; overlapping or reverse ranges retain separate write dependencies. This uses the existing
finite Copy window without sorting, a temporary allocation or a second submission path.

Optional upload timestamps use bounded Query slots. The CPU resets only reusable pairs; Copy queues write the
timestamps, and existing Graphics upload submissions resolve them after the required GPU dependency. Sampling
does not add submissions or CPU waits. Occupied pairs cause skipped samples, reported by `gpu_samples_skipped`;
`gpu_ns` is the sum of measured samples, not an estimate of unmeasured uploads. Results are read only after the
resolving Graphics submission completes. The default upload path has measurement disabled.

Physical release transfers removed resources into the Session's single cleanup scope. Conditions are captured and
registered for all relevant physical Queues in one device-thread segment. Equal completion values preserve
registration order; callbacks are unlinked before execution and may destroy themselves. Arena ranges register
before their Segment's final destruction. Arena, Storage and Context do not create additional cleanup scopes.

Shutdown closes admission, requests Command stop, continues CPU/RenderThread/GPU progress, settles response delivery,
and closes the Responder. Logical resources then register physical cleanup; cleanup and producer callback tails
finish before device facilities are destroyed. Scope join alone does not prove an external producer's stack ended.

## Submission and frame progress

`SubmissionFrontiers` binds its values to one stable DeviceSession and that session's immutable physical queue
indexing. Construction fixes the queue count; accept, merge, clear and reached checks do not allocate. A confirmed
submission contributes a domain-qualified `SubmissionPoint`. Foreign domains and out-of-range queues cannot mutate
another frontier, including in NDEBUG builds. Logical Graphics/Compute/Transfer roles resolve to actual queues first.
Uploader, History, program retirement and frame batch waits use this same value type. GPU completion is still
observed only by SubmissionQueue; a frontier proves neither CPU borrow completion nor WSI completion.

The private `FrameProgress` leaf acquires only `frame_index % frames_in_flight`. A busy next slot applies backpressure
without consuming an index or selecting another slot. Its embedded Round borrow spans Prepare through Server adoption
of native results, independent of Record worker count. Accepted submission prefixes remain in the slot after failure.
The later FrameOperation/DeviceEntry composition must embed and retain that Round; the existing FrameExecution still
serves the current Attachment lifecycle. No second frame pool or GPU completion engine is introduced.

## Complete Resource preparation contracts

`ResourceBinding.hpp` exposes the complete-snapshot preparation contract separately from the current graph-binding
callbacks. Each qualified binding carries a ResourceInstanceId, its professional freezer and the physical capacities
reserved by that Resource's owner. `Preparation` first deduplicates by ResourceInstanceId, checks all requirements and
the round's total staging-byte/range budget, then freezes the whole set. Conflicting bindings for one ID are rejected;
different declarations using the same implementation remain separate. A freezer cannot consume more than its checked
quota. The binding and frozen-view spans have matching indices for the later native assembly.

`ReplicatedData<T>` owns one fixed-capacity CPU table and per-slot dirty sets and immutable CPU snapshots. Storage still
owns native buffers/descriptors. Construction reserves metadata, CopyRanges and snapshot bytes; update, prepare and
settlement do not allocate. A freeze copies every selected record before exposing bytes and merges adjacent CopyRanges.
New writes modify the CPU authority and next dirty set while the pending snapshot stays unchanged. Preparation capacity
is conservatively bounded by the full table byte extent and one range per dirty record; growth belongs to the owner at
a safe point before freezing, not to a partial per-frame upload loop.

`ResourcePreparation` is the real snapshot borrow. Before native submission it may cancel and restore staged dirtiness,
without losing updates that arrived meanwhile. After `beginSubmit`, the operation must deliver the actual result:
accepted commits this snapshot only, known rejection restores pending dirtiness, and unknown acceptance quarantines the
prepared group and its data. Quarantine cannot be reset into a retry and propagates `kOutcomeUnknown` to the future device
execution owner. An in-flight snapshot cannot move, cancel, or disappear with its Storage. Acceptance ends the CPU source
borrow; Uploader/FrameProgress must independently retain native staging and destinations through actual GPU uses.

These contracts are compiled leaves for the later Uploader/FrameOperation and professional Storage migration. Existing
Instance/Light/Material dirty-window callbacks remain the sole active path until that migration replaces them; the new
leaves are not attached as a second dirty/accepted authority. Large Mesh/Texture upload tasks keep their multi-window path.

## Output identity and swapchain flight contracts

`JobOutput` stores only Target identity, physical image index, replacement epoch and publication frame. It owns no
Image/View. `OutputImage` is metadata for a Target's existing physical image entry: one content stamp and the accepted
GPU uses of every reader and writer. Identity comparison includes the replacement epoch, so native pointer reuse after
Resize cannot revive an old output. A write attempt never reuses a previous frame stamp, including after failure.

`OutputWrite::reserve` selects the physical destination before waiting for existing CPU readers and GPU uses to drain.
This reservation blocks new latest-output readers without invalidating the old pixels. Cancelling it reopens reads;
starting the actual write invalidates the old stamp. Planned consumers may hold a CPU borrow during producer recording,
but cannot accept their submission until the producer's required submissions have been adopted and published. A failed
write retains its accepted prefix in the image's frontiers and leaves its content unpublished. Physical entries may
move only while no CPU read/write operation borrows them; moving transfers the metadata without owning native objects.

`SwapchainFlight` associates one Target-owned Acquire, Submit, Present and retirement path. Each real connected native
operation embeds a stable `Operation` node through Server delivery. No Job or frame slot is needed for pre-Run Acquire:
a successful late result becomes READY for the same Target's next eligible writer. NOT_READY publishes no acquired
index; a Present not yet enqueued retains its image/signal association for retry. Failed or unknown submissions require
retirement, not replay. Present acceptance and GPU completion never authorize native WSI object destruction; only the
backend retirement result settles the flight. Destroying an unsettled flight or live native operation is fatal in all
build configurations.

These private leaves are compiled control contracts, not yet connected to the native Target implementation. Existing
SurfaceTarget presentation/retirement and Attachment consumer relations remain the active runtime path until Target
composition replaces them together. No second live Target table, image owner or last-valid manager is installed.

## Job control contracts

`JobCommands.hpp` declares creation, start, stop, removal, dependency/cadence changes, exact execution requests
and Effect rebinding. These values are not yet registered Server handlers; the current Attachment API remains the
runtime entry point until Job composition supplies its actual owners and execution handlers.

The private `JobControl` leaf borrows stable Start, Run and Request nodes owned by connected operations.
Stop closes admission and cancels an unadmitted request, while an admitted Run retains its exact request until
its real submission result is adopted. A cancelled Start remains a deletion-blocking borrow until its candidate
returns. Writer reservation ownership transfers from successful Start to the control; each release clears that
owner's pointer, so repeated Stop or late candidate cleanup cannot release a later writer. The Target reservation
flag must remain stable through this borrow; native image/flight retirement is a separate responsibility.

Only one exact request is pending per Job; a second receives Busy. A request arriving during an earlier continuous
Run belongs to the next Run. The terminal callback executes after the operation detaches all Job bookkeeping and
may release the Job. No Task/Future table or execution-pointer-based lifecycle inference is involved.

`JobRelations` stores the authoritative predecessor and successor sets in each Job. Cold algorithms borrow a view
of the existing table, validate both ends and device identities, then sort the complete logical graph before
filtering the running subset. Stopped intermediate nodes therefore preserve transitive ordering. Dependency changes
prepare adjacency and order together and publish via non-failing swaps on the control thread; rejection preserves
both. Start composition must call the same sorter before publishing admission. No per-frame topological sort is needed.

## Build

Use the existing Render bootstrap or the product configuration. Build `all -j 4 -- -k 0`; after configuration changes
the second build must report no work. The formal sample uses the installed public surface. The explicit output
copy diagnostic uses project GPU contracts and is not a production Readback Command. Whole-product, platform and
performance qualification remain separate from successful compilation or an individual sample.


## Deferred material composition

`compileMaterial(graph, purposes)` and the Cook entry points require explicit purposes. Forward supports Unlit;
GBuffer supports Unlit and metallic-roughness PBR; Shadow computes only AlphaMask coverage. Unrequested uses are not
compiled, and unsupported Forward lighting/Toon requests fail. Lighting and shadow-map sampling belong to features.
`MaterialProgramSet` owns the selected fragment uses, one reflected material record, parameter defaults and raster
state. Shader ABI 4 removes implicit mesh transition varyings; re-cook older material assets. Render consumes
compiled metadata; it does not recompute the material record from a parameter count. The shared shader build inputs live in
`modules/function/material/shaders`; the Material compiler remains a tool/test dependency, not a Render dependency.

If none of the requested uses reads a material record, the Cooked runtime parameter/texture lists are empty.
The author graph retains its slots; compiling another purpose can consume them later. Storage never invents
a layout for unused authoring data or creates a hidden Shader use to recover one.

The private GBuffer, deferred lighting and tonemap Features compose the built-in lit-scene capability.
Connect the GBuffer's `albedo_metallic`, `normal_roughness`, `emissive_ao` and `depth` ports to the lighting feature,
then its `hdr_color` to Tonemap. Port encoding identities supplement size/format matching: two equal-format images
with different channel meanings cannot be interchanged. GBuffer is linear RGBA8 + RGBA16F + RGBA16F and forward-Z D32.

`GBufferFeatureConfig::groups` holds each mesh/material Draw description. Position, UV, world normal and tangent
requirements come from the chosen Cooked fragment's stage inputs. Lit geometry supplies normals; normal maps also
require a tangent including handedness. Nonuniform and mirrored transforms use inverse-transpose normals and
orthogonalized linear-transform tangents. Double-sided state and explicit front winding belong to the group/material.
AlphaMask uses the same Cooked coverage in GBuffer and Shadow; transparent BLEND is rejected by this path.

Each feature's `input_layout` describes the application's attachment parameter block. When composing them, supply
one common block layout to all features; each pass selects only its own named fields. Mesh consumes view/projection,
lighting consumes inverse view-projection, camera position, normalized viewport, background and explicit environment,
and Tonemap consumes exposure in EV stops. Runtime and View do not own a Camera or infer these values.

`LightResource` supplies Scene-local directional, finite-range point and spot arrays. Client Commands
borrow unique input records through Response and return stable LightIds. Dense GPU arrays keep a GPU-format replica
source and dirty ranges for the actual FIF slices; removal updates the moved record's index. Upload and replica repair
use ResourceUploader. Unchanged frames do not create light Senders or scan all light records on CPU.

DeferredLighting remains the all-lights reference for Unlit and metallic-roughness PBR. With no shadow configuration
it compiles the unshadowed path, with no dummy map or sampler. It has no hidden ambient term; AO affects only the
explicitly supplied environment term. Tonemap applies ACES and an explicit linear/sRGB output choice.

## Shadow composition

The private shadow-map, visibility and mesh-shadow Features compose the selected lit-scene implementation.
None installs or calls another Feature. Their common inputs are plain geometry, light and resource-port contracts:

- ShadowMap contributes zero raster passes. Its Attachment data calculates explicit directional cascades, six point
  views or one spot view, plus dense light-to-slice ranges. It supplies `views`, `slices`, `light_ranges`, `raster_states`
  and the same `directional_lights`, `point_lights`, `spot_lights`, `light_counts` snapshot consumed by lighting.
- MeshVisibility reads `views` and the selected instance/geometry tables. Graph Reset/Cull/Compact passes produce
  `gN.visible`, `gN.commands` and `gN.counts`; the CPU never generates final visible counts. Candidate capacity is an
  explicit compiled workload limit; overflow fails preparation before drawing instead of silently dropping instances.
- MeshShadow reads `views`, `raster_states` and each group's visible/command/count ports (`gN.members` is its visible
  input). It alone declares and writes `depth`. Shadow-purpose materials provide opaque/AlphaMask coverage.
- Set `DeferredLightingFeatureConfig::shadows` and connect the four light ports plus `shadow_views`, `shadow_slices`,
  `shadow_ranges` and `shadow_depth`. Its immutable comparison sampler is installed through the ordinary Attach path.
  Geometry groups and view counts must agree across the explicit connections; there is no sibling-Feature lookup.

`ShadowMapShape` selects equal-resolution 2D-array slices, one layer per view. It never reduces quality or drops lights
to fit a hidden atlas budget. Cascades use absolute camera-view distances; their last distance explicitly ends shadow
coverage. `ShadowCamera` is supplied at `camera_offset` in the attachment parameter block. Runtime owns no Camera.
The projection uses forward Z in [0,1] and the selected positive-height viewport; sampling uses the same Y convention.
Raster winding is declared in framebuffer coordinates, including any mirrored instance group.

The directional segment fit, texel snapping, perspective basis, frustum half-space and compact-to-indirect algorithms
derive from the frozen Legacy implementations. The new version uses explicit caster-distance extension and bias units,
without Legacy's split-unit guessing, automatic tile shrinking or empirical bias scaling. World normal offset, normalized
comparison offset and raster constant/slope bias are separate. PCF uses a bounded footprint and guarded UVs; it does not
evaluate screen derivatives inside divergent light/cascade branches. Mesh bounds are supplied in `MeshDescription::bounds`;
missing bounds mean conservative visibility. Transformed AABB support handles mirrors and nonuniform scale.

Preparation resolves multiple outputs in port order, copies only changed projection data into the reusable FIF slice,
and returns before recording begins. Graph passes perform GPU culling and rendering, with declared barriers and queue
dependencies. Raster state is a prepared parameter input; the Executor applies its viewport, scissor and depth bias
without knowing shadows. Every Attachment/FIF slice has independent writable shadow images. Unchanged CPU projections
still redraw current depth; a cache hit is not a proof of valid GPU contents. Physical release uses the existing single
Session cleanup scope. Clustered lighting composes through the ports described below. EVSM, HZB occlusion,
deformation bounds and VSM/Nanite remain separate capabilities.

`render_deferred_scene` retains the CPU lighting reference. `render_shadow_scene` uses public Commands and independent
Feature registration, then test-owned Graph Copies to check actual GPU visibility, shadow pixels, projection data and
AlphaMask coverage. These output probes do not add a production Readback command.

## Clustered light composition

The private `ClusteredLightingFeature` contributes Reset/Build/Count/Scan/Fill graph work;
it does not call DeferredLighting, ShadowMap or GBuffer. `ClusterGrid` declares XYZ cells, linear/logarithmic depth
slices and an explicit index capacity. `ClusteredLightingParameters` supplies the finite forward-Z inverse projection,
camera position/near distance and unit forward/far distance through the attachment parameter block.

With `external_lights=false`, ClusteredLighting resolves its Scene `DirectLighting` storage and exposes the four light
ports. With `external_lights=true`, connect the same light ports supplied to shading and shadow projection. Connect
`grid`, `offsets`, `indices`, `status` to DeferredLighting's `cluster_grid`, `cluster_offsets`, `cluster_indices`,
`cluster_status`, and set `DeferredLightingFeatureConfig::clusters` to the same grid shape/capacity. Lighting then
receives its four light arrays through ports too. No feature is discovered by name or installed as a sibling.

Build unprojects each cell's eight world-space corners. Its conservative AABB works for perspective and orthographic
projections without Legacy's projected-radius approximation near the camera. Count and Fill use one sphere/AABB
predicate; a spot cone uses its enclosing range sphere. Directional lights retain their global loop. Lists keep the
same dense point-then-spot order as the full-light path. One writer per cell removes the atomic write-head resource.
Scan reuses Legacy's 128-lane chunked Blelloch algorithm with saturating addition and an always-written sentinel.

An incomplete list is never consumed: capacity overflow, arithmetic saturation or invalid grid projection sets GPU
status. Lighting uses the complete same-frame light arrays in that case; pixels outside the grid also use full lights.
There is no CPU readback/wait for this decision. Status can be inspected with an explicitly requested graph Copy.
Buffers and their barriers, queue dependencies, FIF instances and cleanup remain ordinary Graph resources.

The initial implementation rebuilds transient bounds/lists each execution. Count/Fill cost is proportional to cells
times finite lights, Scan to cells, and normal shading to the selected list. Reset initializes the declared index
capacity. This is not a claim of sparse construction, cached GPU bounds or a measured speedup; specialized performance
and any larger-grid scan/range optimization require equivalent-workload evidence.

`render_clustered_scene` compares actual pre-Tonemap RGBA16F pixels, copies one coherent frame of cluster data for an
independent CPU geometric/list oracle, and tests the production Scan with overflowing counts. `render_shadow_scene 3
--clustered` adds the same HDR comparison to shadows, multiple materials and lights,
including an intentionally tiny list capacity. These checks use the full-light result as a correctness reference,
not as a hidden replacement for the successful non-overflow Clustered path.

## Scene composition and measurement boundaries

`MeshInstanceDrawGroup::material_source` selects a material **program table** through a live material in that table;
it is not a filter for just that MaterialId. A group selects its Mesh/submesh and all matching instances in that
table. Each instance's GPU material index selects its own parameters. Configure one group for that Mesh/submesh/table,
even when many MaterialIds share it. Listing those materials as separate groups repeats the same draws.

Set `GBufferFeatureConfig::visible_capacity` to the capacity of the connected visibility producer. Connect its
`gN.visible/commands/counts` outputs to GBuffer's `groupN.members/commands/counts` inputs. GBuffer does not instantiate
or query another Feature. With capacity zero it retains the all-candidate reference path.

`MeshVisibilityFeatureConfig::view_parameters` optionally supplies the Attachment parameter layout containing
`view` and `projection`. In that mode one Compute pass produces the geometric view and six homogeneous clip planes;
each Attachment/FIF slot has its own ordinary Graph buffers. Without this layout, the composition connects the
`views` input explicitly, as for shadow slices. No Camera owner, CPU readback or extra synchronization service exists.

Visible members, commands and counts contain two streams per view: positive transform determinant, then negative.
Raster Features use the corresponding front-face state, including for double-sided material normal orientation.
`front_counter_clockwise` describes the base geometry/projection, before instance mirroring; callers must not invert
it again for a mirrored instance. Zero-determinant instances are not drawable. The all-candidate path uses two vertex
variants to reject the other orientation without a CPU transform scan; the GPU visibility path partitions members
before drawing. This increases the number of raster orientations/Pipelines relative to a uniform-sign-only path;
it is a correctness change, not a performance claim.

`RenderServerConfig::diagnostics` borrows a `RenderErrorSink` through initialization, compilation and device teardown.
It receives backend severity, a static error identity, message, file and line. Text is valid only during the callback;
the application owns the destination and any retained copies, and handles concurrent calls without reentering the
Server. Error/warning counters remain available even when no sink is installed. Render does not select a terminal,
link a logging library or create a diagnostic queue.

Measure the actual composition: output extent, camera coverage, active and shadow-casting light counts, shadow slices,
resolution/filtering, view count, FIF and update cadence. An offscreen Render measurement excludes presentation and the
rest of the application. All-lights and Clustered modes can be compared with the same scene; no light-count threshold
or automatic mode switch is implied. Overflow fallback must be distinguished from a successfully built list.

Production progress counts accepted executions. Steady-state throughput over many bounded FIF cycles is useful, but
it is not a measurement of individual GPU completion or display latency. CPU record/submit time, GPU timestamps,
Client response observation and profiler sampling remain separate measurements. In particular, a test Client's reply
polling or sleep time is not RenderThread work. A frame-driven Client can keep one borrowed update batch in flight and
consume its response on later ticks before reusing that input.

## Resource lookup and commands

Resource declarations and Storage implementations have separate identities. A Device, Scene or View
owns at most one instance of each declaration. Two declarations can reuse one StorageMeta and factory
without sharing instance data. ResourceSet owns the instances; ResourceRegistry only indexes them.

Ordinary clients include `commands/ResourceCommands.hpp` and `resources/BuiltinResources.hpp`.
They query or explicitly create the desired Resource before uploading:

```cpp
auto resource = co_await client.request<GetOrCreateResource>(
    ResourceRequest{GlobalRenderScope{}, kTextureResourceName}
);
if (resource.error.type != 0) co_return resource.error;
auto uploaded = co_await client.request<TextureCommands::Upload>(
    upload_command, TextureCommands::Upload::Request{resource.resource_id, data}
);
```

`QueryResource` never creates an instance. A Scene/View query can resolve a Resource in its actual
ancestor domain; it cannot select an unspecified View or an Attachment. Existing item IDs contain the
parent ResourceInstanceId and local generation. Upload/create requests use resource_id, while item
updates and releases use the complete item ID; neither repeats an Effect implementation or Scene route.
The request source remains borrowed through the terminal response. A failed execution candidate does
not delete a Resource that has already been published into its domain.

Creation configuration is registered with the Resource, independent of Effect parameters. A non-void creation config
must be installed through `registerResource` before a capability reuses that declaration. Query and GetOrCreate
share declaration/domain/device qualification; Query never constructs. Cold graph compilation resolves names to
ResourceTypeId requirements. Attach reuses that same creation path and retains no extra Resource owner. A failed
attachment candidate does not remove already published resources. Preparation deduplicates ResourceInstanceId.

Different declarations can share the exact StorageMeta and Factory while owning separate instances and Command
associations. Repeated registration must agree on policy, creation defaults and methods. A resource command's
instance selects its declaration first, then its exact thunk; implementation type only qualifies the final borrow.
Stateless resource methods allocate no Handler object. Admitted asynchronous commands retain ResourceCommandUse
through terminal delivery; Scene/View removal returns Busy while such a borrow exists. Removal revokes identities
before destruction, and dense record moves repair the index. References are allowed only within the same actual
owning domain or its real ancestors on the same device.

The remaining Attachment/FrameExecution lifecycle continues to serve its existing execution responsibilities.

## Native execution and return delivery

Each DeviceSession owns one NativeExecution host for its program-creation and queue-submission serial domain. Logical queue roles
share that host. It combines the real ManualContext, a stable Producer, a retained atomic wakeup and one worker;
it defines no replacement Sender or completion protocol. Only the control owner obtains and starts ordered native
work from the Server delivery scheduler; standalone consumers provide their own single control owner.
Scheduler access does not claim to verify the thread that eventually starts an operation.

Static and dynamic pipeline preparation freezes its inputs in the connected coroutine operation. Shader preparation
runs on the CPU pool; NRI pipeline creation runs on the native worker. Native value, error and stopped terminals are
normalized before delivery to the originating control scheduler. Program-cache publication and business-state changes
happen after that return. Installed command/frame dispatch contracts carry only the real Server delivery scheduler.

Shutdown settles admitted scopes while Server delivery remains open, closes and joins the native host, then destroys
native dependencies. Joining includes callback-stack and publisher tails, even when the logical operation has already
reported its terminal. Closed native scheduling reports a structured error on the starting thread; rejecting required
return delivery is a fatal host-lifetime violation. The wakeup storage outlives its Context and worker. Context allocation
capacity is a hint; actual Frame, Job, Uploader and Queue admission limits remain their owners' responsibility.

Queue submission freezes its descriptors in the connected task and permits one unconfirmed call per physical queue.
Only successful native return advances the submitted frontier. Known rejection preserves earlier accepted work;
uncertain acceptance enters a terminal UNKNOWN state without replay or an invented completion value. Only this
exceptional path uses native QueueWaitIdle to retire uncertain payloads before delivery; ordinary reuse observes fences.
The external NRI fork propagates D3D12 Wait/Signal failure and classifies potentially accepted submission as uncertain.
ResourceUploader progresses without a Job or FrameSlot. CPU source borrowing ends at the task terminal while accepted
staging and command windows remain protected by their real GPU frontiers. Stop still adopts an already accepted
native result before returning cancellation. Server drains both frame/command and upload work while return delivery
is open. WSI integration remains on its existing path until the Target task replaces it.
Core RunLoop consumers outside Render remain supported.

## Target shader artifacts

Graph and material inputs now use the shared `rdesc::ShaderProgramArtifact` and
its target identity. Each binary carries its own reflected layout; compiled
program identity includes the target. `MaterialProgramSet` holds a single target's
programs and parameter layout. The module-level Slang compiler can derive SPIR-V
and DXIL without linking Render or reading an author graph.

All built-in shaders use the shared, pinned Slang compiler and target-specific reflection.
`lux_shader_compile ... bundle ...` embeds both SPIR-V and DXIL. Render selects the
artifact matching the actual device; mismatched or absent artifacts are rejected.
Vulkan relocates descriptor decorations, while D3D12 preserves the compiled register
space/index through the NRI descriptor-range mapping. Root constants use `b0, space255`.
On D3D12, shared descriptor tables are CPU copy sources. The writable FIF snapshots live
entries into its own GPU descriptor pool alongside private sets, so one heap pair supplies
all bindings. Table growth rebuilds that frame's pool and invalidates affected recordings.
DXIL never passes through SPIR-V relocation. Runtime validation uses pinned SPIRV-Tools
and the resolved SPIRV-Cross package; generation has no Engine toolchain or PATH/SDK fallback.

D3D12 requires Shader Model 6.6 and loads the deployed Agility SDK beside the Render DLL.
It uses the existing contiguous instance-buffer path, with per-draw member offsets when
first-instance semantics are unavailable. Vulkan's optional physical-pointer paged variants
remain SPIR-V specializations; requesting buffer device addresses on D3D12 returns unsupported.
Both backends retain the same ordinary mesh, shadow, lighting, postprocess and UI features.
This backend integration does not establish WSI, rendering-output or performance qualification.

ImGui is pinned to fork commit `ad75bbcb15ba47f921dca082b1377cca9907ae98`
(1.91.9 WIP, 16-bit ImDrawIdx, 64-bit ImTextureID). Lux owns UI GPU uploads and
encoding; NRIImgui remains disabled. A later UI migration must replace that owner,
not add a second active upload path.

## Mesh and Texture Resources

`resources/mesh/Resource.hpp` and `resources/texture/Resource.hpp` declare the public Resource contracts.
Their sole implementations, packing and command adapters live under `resource/mesh` and `resource/texture`.
Command registration binds static adapters to a qualified instance; there is no empty Handler object or
capability-selection upload route. Each public professional directory groups its `Resource.hpp`, `Commands.hpp`,
`Data.hpp` and extension-only `Binding.hpp`; generic GPU binding contracts remain in `extensions/resource`.

Mesh packing runs on the shared CPU scheduler and returns to the originating control scheduler before native
allocation or publication. Both factories require an Uploader, but no device frame slot or frame-count policy.
Entry identities retain their parent Resource; declarations sharing implementation metadata still own distinct data.
Failed or stopped uploads retire only their private candidates. Existing BufferArena and Session cleanup frontiers
protect accepted GPU work independently of the CPU Response.

Texture uploads retain format/extent/mip/colour validation against the actual device. CloneRegion produces a new
identity, preserving the source, its untouched mips and regions. The source entry carries its actual clone borrow
count; Release returns busy until those native submissions have returned. Null bindings retain the white default,
while stale or foreign non-null identities fail explicitly.

D3D12 COPY uploads hand off through COMMON (`nri::Layout::GENERAL`) before Graphics transitions to shader access,
using the existing queue fence dependency. This follows the
[DirectX COPY-queue layout contract](https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html#copy-queues).
The pinned external NRI fork rounds explicit BC copy extents to complete blocks, consistently with its existing
whole-mip path, for upload, clone and readback of tail mips. Vulkan keeps its logical texel extents.

## Material Resources

`resources/material/Resource.hpp` declares the Resource; `resource/material` owns its single material table and
static typed command adapters. Inputs own shared target programs, parameter values and TextureIds. Render does
not retain author graphs, source pointers or Slang sessions. Each program set must match the device target and
its reflected record layout. Requested purposes must exist explicitly, including opaque Shadow programs.
Canonical empty records from constant or Shadow derivation are valid.

Default values do not distinguish table structure. Dynamic updates retain the table slot and do not compile
programs; a new target program creates a separate material identity. Native pipeline variants are prepared on
demand and published only after candidate success. Failed writes retain published CPU records and mark affected
GPU replicas for repair before use; Stop does not publish an accepted upload's cancelled candidate.

Texture references must name live entries in the same Resource domain or a real ancestor. Null explicitly selects
the white default. Clients must end material references before releasing textures, and end draw references before
releasing materials; this does not add generic reference counting. DeviceSession frontiers retire GPU objects.
Engine's decision to derive a new purpose remains separate integration work.

## Scene, View and UI data

`resources/instance/Resource.hpp` and `resources/lighting/Resource.hpp` declare Scene-owned data. Their implementations
and static command/frame adapters live in `resource/instance` and `resource/lighting`. Use
`FrameData::appendData<InstanceResource, TransformUpdate>()`, the `MeshInstanceUpdate` column, and
`FrameData::appendData<LightResource, DirectLightUpdate>()`. Targets resolve during Attachment construction.
Standalone `InstanceCommands::Update` and `LightingCommands::Update` use the same algorithms without an Effect.
Empty spans mean no update. Dynamic entries are checked before mutation; a multi-Resource packet is not a rollback
transaction. Mesh/material associations, regrouping observers and buffer-growth subscriptions remain
active. Clients end those references before releasing the referenced entries.

Active graph consumers share each Resource's real FIF slices. Structural changes rebuild a list deduplicated by
ResourceInstanceId; preparation tracks the source revision separately for each physical slot. Failed preparation
does not acknowledge dirty data. Accepted replacement buffers publish to all subscribers before subsequent use.
There is no Job-owned copy of the Scene transform table or second dirty/accepted owner.

Use `UpdateViewCamera` with a ViewRequest and CameraInput. Each View owns its camera and revision, including before any
Effect is attached. Attachment creation and frame preparation project that View value into the selected shader
regions; these are execution copies. Effect configurations retain algorithm parameters. CameraInput is no longer
accepted as an attachment parameter update. Engine camera controls and the later Job/Effect control migration are
separate work.

UI processing lives in `resource/ui`. `SubmitUiFrame` carries explicit borrowed vertex/index/command spans plus
an optional RenderFrame column packet. It enters the same FrameSubmission control and holds all UI sources through
terminal upload consumption. An explicit empty UI snapshot can clear draws; an empty Resource column cannot clear data.
GPU staging and FIF reuse still follow Uploader/Queue frontiers. UI layout checks and target-specific padding remain.

## CPU input columns

`FrameLayout` describes channels by Resource declaration and element type. Construct its metadata with
`makeFrameLayout(std::type_identity<std::tuple<Resources...>>{}, execution_revision)`. Repeated declarations
share channels; distinct declarations using the same element do not. The revision belongs to the actual
execution owner; capacity changes do not change it. Copies share immutable metadata identity, so frame admission
compares identity rather than repeating ABI/name qualification. Layouts contain no client data or Storage pointers.

`FrameData::create<Resources>(layout, initial_capacities)` checks the complete typed contract and fixes its
column directory. Each column owns one typed vector. `appendData<Resource, T>(count)` grows that column and
returns only newly constructed elements; `addData<Resource>(value)` appends one. `getData<Resource, T>()`
returns only valid const elements. Fill an appended span before sealing; growth of the same column invalidates
its old spans. Other columns and another CPU slot can grow independently. Reset retains capacity; an empty
column means no update, and cross-column ordering does not express last-writer-wins semantics.

`seal()` makes the owner immutable. Writes/reservations reject a sealed packet; reset, move and destruction
are fatal ownership violations. RenderClient acquires an exclusive packet borrow at operation start and returns it
on the caller thread before delivering the real terminal response, including pre-publication failure and host settlement.
A second request cannot borrow the same in-flight slot. Sending or cancelling an accepted request does not return it.
Raw IPC callers must retain the sealed owner and call `release()` only after the actual response or host settlement.
Element metadata and code remain loaded until the packet owner is destroyed.

Obtain the live layout with `QueryFrameInputLayout(attachment)`, initialize two FrameData owners from it, fill one,
seal it and send `RenderFrame{attachment, &packet}`. Reuse only the returned slot. Capacity changes do not affect the
layout identity. Replacing the Attachment publishes a different identity; old layouts fail before applying anything.
The current Attachment owns these cold bindings until JobExecution takes over the same responsibility.

## Shared copy recording and preparation settlement

Graph transfers, staged asset/table uploads, texture clones and Feature buffer preservation use the physical
recording functions in `extensions/device/CopyEncoding.hpp`. Callers supply the exclusive command buffer and
qualified resource ranges. The shared texture encoder expands mip/layer regions; upload/readback retain the
qualified native row and slice layout. Graph compilation still owns graph barriers; Uploader owns its transfer
and queue-family handoffs. The encoders neither allocate resources nor submit or wait for GPU work.

`BufferCopyRegion` is a physical copy value independent of ResourceUploader. Feature preservation is recorded
by the frame owner through `recordPreservingCopies`, including its existing source/destination transitions.
Uploader continues to own staging pages and transfer command windows; GraphInstance owns graph command resources.
Both submit through the existing SubmissionQueue and NativeExecution with the same accepted/rejected/unknown facts.

Each physical frame owns a `Preparation`. Resource and Feature preparation receive it alongside the actual
Uploader, and register replica settlement with `Preparation::onSubmitted`. Uploader first commits/cancels its
staging ranges; the frame owner then delivers the native result through `finishCallbacks`. Callbacks appended
during delivery wait for a subsequent settlement. Reentrant settlement and destruction with pending callbacks
are ownership errors. The existing frozen ResourcePreparation API still settles its snapshots explicitly.

Write-window admission, source-byte credits, background-transfer gating and staging reservations remain in the
Uploader while the active FrameExecution publishes the physical slice. These are live admission/lifetime facts,
not another submission-result authority. No replacement owner currently takes over those gates. Standalone
Mesh/Texture uploads still progress without a running Job; each accepted prefix keeps its real queue frontier,
CPU sources end at their operation terminal, and staging pages wait for their GPU completion callbacks.

## Device-independent Feature construction

`FeatureBuildContext` exposes the unique graph Builder, local `resource`/`publish` port operations and `require`
for qualified Resource configuration. Mutable argument and preparation collectors are private to assembly.
Samplers enter `RenderGraphBuilder::addSampler` and belong to the resulting graph. The active professional
preparation bindings remain internal until graph-instance execution takes over their real lifecycle.

`GraphProgram` retains SPIR-V and DXIL artifacts with their own reflection. Logical construction verifies that
both targets expose the same named resource/access contract; it does not equate their physical offsets or registers.
Device compilation selects the target and resolves binding names against that target's reflection. Embedded shaders
are already compiled build outputs, so loading both artifacts does not invoke a compiler during construction.
Device-dependent constants, such as indirect-first-instance support, are qualified by reflected field name and
filled during native program preparation. They cannot overlap ordinary Resource/View parameter projections.
Explicit `zero_fill_tail` constant bindings initialize only the supplied byte prefix and zero the selected target's
remaining reflected bytes; ordinary exact-size bindings remain strict. UI and fullscreen passes use this for
target-dependent constant-block padding.

`CapabilityImplementation.hpp` is the installed implementation declaration. The mutable CapabilityRegistry is a
private runtime directory, with no public forwarding header. Native Plans belong to execution candidates, not Effect
records. An accepted candidate retains its Effect recipe until delivery; Scene/View/Target generations, target
resize state and replacement eligibility are checked again after asynchronous compilation. A failed candidate leaves
the old writer intact. Stop settles the accepted operation before the DeviceSession can close. The remaining
professional preparation protocol still requires the graph/execution migration.

`prepareFrame` copies parameter values into the selected FIF slice and freezes the physical resources, pass
inputs and History initialization commands used by `FrameRecordInput`. `FrameExecutor` only borrows those
stable slices during synchronous recording; it has no GraphInstance access and performs no texture-description
queries. Changing author parameter bytes after preparation affects the next preparation, not this recording.
Physical binding or recording invalidation requires preparation again. The caller retains exclusive access to
the FIF data through recording and waits for accepted GPU uses before reusing its command allocators.
