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
across selected capabilities. `ListCapabilities` discovers concrete implementations by capability type name
and implementation name. Results contain the public declaration, implementation identity, Feature IDs and
device eligibility. Each implementation lists exact output format, sample count and Target kind combinations.
Target and Graph qualification handle sampling separately.
In an Effect with selected implementations, each Feature key belongs to exactly one selection through an exact
name or slash-delimited child. Its Feature ID must be listed by that implementation, and each live external
Resource must be declared by one of the selected Capabilities. An Effect without selections obtains its
Resource requirements from the Feature ports.
The saved composition keeps names and actual configuration bytes; generational IDs belong only to this Server.

Resource upload is independent of Effect creation. `GetOrCreateResource` selects an instance in a real
domain, and create/upload commands carry its resource_id. Item commands carry complete parent/local IDs.
`CreateEffect` selects the graph composition and output contract. FrameData identifies a Resource declaration
and element type. Job assembly resolves the graph's declared Resource requirements.

Views share an immutable logical Effect and the device program cache. Each execution owns its native Plan
and graph instance. Mesh/material IDs, grouping, shadow
layers and UI images are live instance/frame data, not Effect compilation inputs. Late material programs prepare
on the shared CPU scheduler and are adopted on RenderThread before recording a frame. The Job receipt reports
the actual submission outcome. Already adopted Resource inputs remain in effect and are applied only once.
DeviceSession's cleanup scope retires accepted physical uses.

One dynamic Raster pass consumes zero or more prepared Draws/Pipelines. Its per-FIF arrays, descriptors and
parameters belong to the instance. Actual external reads include sampled Target images, material texture tables
and BDA-addressed buffers; binding changes rebuild the combined submission dependencies before recording.

Resource definitions declare their Commands. Each Resource registration supplies the corresponding static typed thunks;
CommandRegistry owns those calls and ResourceRegistry records their locations by declaration and CommandId.
Capability `Bindings` contains only independent capability methods. Mesh upload consumes `rdesc::Mesh`; clients do not produce Native GPU layouts.
Handlers use the professional Storage Senders and ResourceUploader. Transform and light elements use
Resource-declared `PerFrameData` in one frame batch. Camera uses a View command. The independent Editor UI
extension supplies its own snapshot command. Resource inputs never generate per-item Requests or Responses.

A module registers each Resource, Capability, Feature, Handler and static method once using the same setup
APIs as builtins. `register` accepts a new definition; duplicate identities are errors even when the supplied
payload is identical. Keep the returned IDs, or query the catalog on its owner, to reference existing definitions.
Each successful registration is its own transaction; install shared dependencies once before publishing implementations.

An implementation declares `using Capability = ...`, `Features` and `Bindings`.
`registerCapabilityImplementation<I>()` references registered dependencies and their exact method locations.
The implementation supplies its own contribution state factory when it exposes Effect commands.
Resource commands share one protocol record and retain
separate call locations for each Resource declaration, even when they use the same Storage class. The directory
records each implementation's parent CapabilityId; its own ID is already the container key. Cold add/replace/remove
calls reject active execution and invalid entries; replacement creates a fresh generation. Resource configuration
is immutable after registration. GetOrCreate reuses the same-domain instance by ResourceTypeId without comparing
that unchanged configuration. Callback/type metadata must outlive the Server and all accepted work.

One capability can use several graph Features. The built-in Lighting3d implementation connects shadow projection,
shadow rasterization, Clustered Lighting, deferred lighting and Tonemap inside one EffectDefinition. These algorithms
are not separately advertised capabilities. `DeferredLightingConfiguration` owns their actual switches/settings;
`ClusterGrid`, shadow settings and color encoding are data, not additional configuration services.

`CreateEffect` accepts one owned `EffectDefinition` and optional implementation identity references. The typed
`makeDeferredRenderEffect` and `makeUiRenderEffect` author functions form defaults, Feature uses, connections and
actual output references before submission. Extension authors construct the same definition. Server performs one
`composeFeatures` pass; it has no Capability port/shape solver, graph callback, default factory, configuration decoder
or `ValidateCapabilityConfiguration` command. Saved composition bytes remain author data: the owning module decodes
them to its concrete configuration before calling its typed author function. Existing files/codecs are retained.

Feature registration is device independent. Address-based Mesh programs are retained as logical program alternatives
and selected during device Compile against the Device's resource representation. Material draws select their encoding
during native preparation. Both continuous and BDA Mesh algorithms remain; D3D12 consumes its DXIL/reflected layout.

Camera inputs belong to View and exposure belongs to Effect. The external UI extension submits its own Resource
command and uses the generic indexed draw and sampled output contracts.

Command/configuration/binding identity uses `cxx::basic_type_info` from `make_basic_type_info`. It is C++ type agreement for
compatible builds, not cross-compiler serialization. Actual file codecs preserve their own format identity. Type metadata,
callbacks and destructors must remain loaded until every dependent operation/value has been cleared.

Ordinary Features declare `Config`, `Ports::describe` and `build`; registration generates their erased
`FeatureGraphOps` thunks. `Ports::describe` exposes config-dependent port information without building a graph or
creating device objects. Composition calls that same cold entry. No Feature object or empty runtime hook is required. Imported ports carry their initial use; selected exports
carry their exact range and final use. Concrete graph building remains in the project-level Feature implementation.

## Requests and execution

Framework commands are split by Scene, View, Target, Effect, Job, Discovery and Server responsibilities.
`commands/Commands.hpp` is their aggregate. A Job owns its logical Scene, View and Effect relationship from Create
to Remove. Start selects the Target and builds a private device execution. Stop closes future frame admission and
returns writer eligibility while an admitted Run retains its execution through native acceptance. Remove requires
those borrows to settle. Rebind checks a candidate before publishing a new Effect or Target on the same JobId.
Mesh and Instance commands have independent namespaces under `resources/mesh` and `resources/instance`.

The host serializes catalog setup before calling `run()`. The call seals registration and transfers the
catalog to the Server thread through shutdown. Runtime clients discover IDs through the fixed
`QueryCommandIds` bootstrap; `RenderClient::create` uses that IPC path. Accepted commands use IDs and retain
the binding qualified for the actual Resource.

A Resource command declaring `requires_idle_resource` is exclusive for that instance. A conflicting
operation returns Busy, and Jobs that need a Resource with an exclusive command in flight wait for its
terminal delivery. Unrelated IPC, Jobs and transfers can proceed.

Use `registerCommands` with `CommandBinding::declare<C, function>()` for stateless typed sync/async functions.
For a request with an `effectId` selector, declare its synchronous method with
`CommandBinding::declareForEffect<C, State, function>()` and select the corresponding `EffectCommandMethod`
in the implementation's `Bindings`. The implementation supplies `EffectStateFactory::declare<State, create>()`.
After successful graph assembly, `CreateEffect` creates one state per selected contribution on the Server
owner and links each full command ID to its method and state. It rejects two contributions claiming
the same command, including repeated selections of one implementation. The typed method receives its
state by reference and acknowledges CPU updates. Graph preparation and GPU parameter publication run separately.
Stateful Handlers retain constructor-injected dependencies. All forms use the existing request operation,
Resource borrow and ResponseOutbox; an async request's source bytes remain borrowed until its real response.

`JobExecution` owns CPU input bindings and Resource subscriptions. The graph instance owns the derived
`InstanceData` objects created from immutable graph recipes and the actual per-FIF GPU bindings. Cold assembly
resolves Resource borrows once; instance preparation uses those borrows and contributes copies to `Preparation`.
After Target prefetch, FrameProgress opens the next strict FIF slice for the ready Jobs. FrameOperation prepares
Resources and instance data and submits preparation copies before graph work. Destruction releases derived objects
in reverse order. Physical cleanup uses the Session scope.

Each FIF command batch owns its allocators, command buffers and recording versions. A batch recording lease rejects
concurrent recording of the same batch; independent batches have separate allocators and per-pass scratch. The caller
must join record workers before preparing, resizing or destroying the instance. The round retains the slice until
every accepted queue frontier completes; a device terminal prevents further preparation or recording. Direct internal
recording callers must enforce the same completion boundary. The recording lease does not replace GPU completion.

`prepareFrame()` copies parameter bytes and freezes resolved native views into instance-owned FIF storage. Client
vector mutations do not alter those bytes. FrameExecutor only traverses the Plan and calls typed encodings with
stable slices; it performs no resource discovery, compilation or submission.

Image/Buffer copies distinguish capacity from initialized content. With fixed dimensions and an explicit row pitch,
only actual row bytes become defined; padding remains undefined. A destination `READ_WRITE` explicitly preserves
an already defined range, including padding. It is required when dimensions or multi-row pitch are resolved only at
instantiation. Count and indirect argument accesses retain their actual offsets and byte ranges even though NRI
Buffer barriers operate on the whole physical buffer.

Graph/plan JSON and DOT describe dependencies, resource ranges, layouts, rendering attachments and barrier provenance.
`SubmissionOperation::Workspace::dumpSubmissions` separately reports the latest accepted batch per FIF slot and current frame queue
frontiers. These are bounded submission facts, not GPU timing or a continuous trace.

Applications use Platform IPC directly. They discover CommandId, attach it with `commandTag()`, send a typed
Request and consume the corresponding Response. Responses contain `RenderError error` and business fields;
zero `error.type` means success. Internal fallible functions use `RenderResult<T>` where an error is recoverable.
The library returns structured diagnostics and never chooses a terminal or logging destination.

## Running a Job from the installed SDK

1. Create a RenderServer, register extensions and run it on the host's render thread. Open an IPC requester
   and bootstrap a RenderClient. The host drives `dispatchResponses()` at its completion points.
2. Send `CreateDeviceSession` with the device options. Pass the returned `DeviceSessionId`
   to `QueryRenderDevice` and `ListCapabilities`. The Server can hold multiple Sessions for device queries.
   `CreateTarget`, `QueryResource` and `GetOrCreateResource` qualify the device explicitly. Scene and View IDs
   describe logical scopes. Each device owns its own Resource instances for those scopes; use `DeviceScope`
   for a device-level declaration. Asset commands keep routing from their parent ResourceInstanceId.
   `RemoveDeviceSession` returns Busy while the device has a Target, execution or active borrow. Once
   admitted, removal closes that device to new work and replies after its accepted GPU work and resources
   have retired. The other devices continue to submit while removal is in flight.
   Upload meshes and textures before any Job exists, and await their typed replies before using the resulting IDs.
3. Create the Scene, View and Target, then use `CreateEffect` with the logical Feature recipe. Create the Job
   with the Scene, View and Effect IDs. Pass the device and its Target to `StartRenderJob` to prepare execution.
   A running Rebind stays on that device. Cross-device Target reads and execution dependencies are rejected.
   Each device owns its frame slots, upload state and published output history. The Server visits ready devices
   in rotating order. QueryRenderDevice reports a device's execution error separately from the query status.
   A terminal device error closes its admission while healthy devices continue submitting.
   Bind a sampled producer Target to an authored external `FeaturePortRef` in `JobBinding::target_images`.
4. Use `QueryRenderJobInputLayout` to construct two stable `FrameData` slots. Seal a slot and call
   `UpdateFrameData{job, &packet}` to apply it without granting an execution credit. Use
   `RequestRenderJob{job, &packet}` to apply it and request one submission. An exact request can omit the packet
   to use persistent state. A continuous Job schedules itself when ready.
5. Reuse a slot on its terminal reply. An update reply confirms input application. A successful exact Job
   reply confirms native submission acceptance. Actual GPU completion governs physical FIF reuse and Target
   retirement. For window Targets, presentation is a separate native outcome.

Both operations use the same typed columns. The Server checks the shared layout identity, Resource identities
and exclusive access before applying nonempty columns once per Resource. If a Resource rejects its input,
earlier successful Resource writes remain in effect. The reply settles the packet borrow. After
`StopRenderJob`, new updates fail with `kJobStopped` while already borrowed packets settle on their own terminal
replies. An exact request that fails has its own terminal
and can be retried after its cause is resolved. A continuous Job with a failed round exposes `last_run_error`
through `QueryExecutionProgress`. An explicit request, execution input change or cadence change can retry it.
The [installed UI consumer fixture](../../../editor/ui/render/test/FramePacket.cpp) drives the SDK and verifies
texture upload, typed updates, exact Jobs, and GPU output without an Engine host.

Synchronous Handlers fill the Response directly in the Outbox message. Asynchronous Handlers return a concrete
Sender. `counting_scope + spawn` owns connected operation states. Errors and stopped completions become typed
Responses, and `continues_on` delivers them to the RenderThread's `ResponseOutbox`. The schedule operation
guarantees this final handoff even during shutdown, preserving failures and unknown outcomes. No separate
result record or hand-written completion callback owns the Response. The RenderThread publishes IPC replies.
Each asynchronous Command, including RequestRenderJob, still has stdexec's operation-state allocation. Allocation-free
steady recording is a narrower claim than allocation-free Client-to-Server frame submission.
The service loop adopts ready CPU results before dispatch, runs continuations made ready by GPU completion/write
admission before frame preparation, then delivers terminals produced by the frame. These three explicit safe points
also remain active while the Command Scope drains. GPU polling itself does not drain the Server ManualContext. The Context uses delivery scheduling and remains
open until accepted operations, publisher tails and required cleanup have settled.
Outbox service counts settled records, including replies discarded for closed destinations; it is not a delivery
acknowledgement. The wakeable idle revisit remains bounded because one IPC receive examines only part of the Links.
The CommandRegistry owns the command directory and installed Handler objects. A framework command has
one concrete binding. Resource commands use their ResourceInstanceId, and Effect commands use their EffectId
to select the binding qualified at Effect creation. An accepted operation retains the installed call it borrowed.

Upload sources are borrowed. After successful submission, the Client keeps their addresses, extents and contents
valid until Response, including failure or cancellation. Failed submission takes no borrow. Abandon or connection
loss does not prove reading ended; the application settles the Server and CPU producers before releasing such
inputs. Native staging and submitted GPU resources remain the device's responsibility.

`ConfigValue` exclusively owns one immutable typed configuration. Its defining code must remain loaded through
destruction. The compile request owns configurations during asynchronous compilation; graph assembly borrows them
synchronously. Dynamic configuration cost counts actual owned capacity, not external upload bytes.

## Graph and Features

The Builder owns graph-local resource versions and strongly typed Raster, Compute and Transfer commands.
Extensions declare their attachment scopes, resource ranges and usage through those commands. The compiler validates
each command and lowers it into a recording block referencing a typed packet table.

Recorded commands have no extension callback or borrowed source configuration. A Feature that produces dynamic draw
data uses the indexed or raster draw stream contracts. Indexed producers resolve the final stream with
`InstanceAccess::indexedDrawStream` during creation. Preparation may supply bounded copies, resource bindings and
draw packets. The graph still declares attachment writes and fixed graph reads.

`RenderGraphBuilder` owns candidate arrays. `finish()` produces one self-contained immutable Definition;
`analyzeRenderGraph()` produces independent analysis data. Graph resource/pass IDs are indices within that Definition, not
global generational identities. Version zero describes explicit initial contents; positive versions name writes.
Reads, read/write predecessors, conditional fallbacks and overlapping ranges are explicit. Exports, published History,
imported writes and explicit side effects root culling. Invalid ranges, live cycles, ambiguous producers and incomplete
reads fail with structured errors.
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

Use `nextWriteVersion` and `addComputeSequence` to declare a read of a preceding dispatch within one logical
Compute pass. The Builder commits the sequence as a unit and returns the final written version of each resource.
Analysis keeps the commands together, derives their hazards individually, and rejects dependency cycles. The compiler
records compatible adjacent work in the same CB and emits the synchronization between commands.

Compute work can declare permission to run asynchronously. The compiler uses a Graphics schedule by default.
A device can supply measured submit and queue handoff costs in DeviceOptions::graph_queue_costs, and Features can
supply workload-specific command timings in GraphPassOptions::estimated_gpu_ns. With complete timings for the live
graph, the compiler compares the Graphics and eligible async schedules using their actual compiled submission DAGs.
It estimates GPU completion across queues, includes native submit costs, and requires the winning async candidate to
preserve the baseline's reuse of identical physical objects. Runtime memory alias placement still uses NRI requirements.
Imported images, surface output uses, sequences, and conditional intervals constrain queue assignment.

`GraphCompiler::compile(session, cpu, graph, analysis, ...)` prepares device-qualified programs, descriptor bindings,
submissions and physical resource layout before publishing the `RenderGraphExecutable`. Shader reflection supplies
layouts and descriptor writes on SPIR-V and DXIL. Fixed descriptor operands and initial parameter bytes are stored
in contiguous tables indexed by pass ranges. Descriptor pool counts and instance parameter copies are computed
during compilation. Separate image and sampler bindings remain explicit. Program preparation returns to the owner
scheduler for publication through the existing `CompiledPrograms` cache. The device execution owns those native program
leases alongside the executable. The executable's recording tables carry slots and the Instance freezes their physical
bindings for the recording workers. Barrier predecessors and History predicates use compiled slots. The diagnostic
sidecar carries source Pass IDs, resource names and reuse reasons.

The Effect keeps Storage requirements and cold Feature creation data. Start or Rebind resolves those inputs and creates
producer bindings before initializing the `RenderGraphInstance`. Factory calls can resolve source Pass IDs during that
assembly step. Layouts and field names are used there to link external parameter sources to byte copies. The initialized
Instance owns the linked copies and allocates graph resources and descriptors per existing FIF slot. Standalone
project GPU consumers can bind physical inputs directly without creating a Registry or Server.

The `RenderGraphExecutable` contains a recording program with typed Raster, Compute and Transfer packet tables,
range transitions and rendering groups. Legacy's consecutive-pass
grouping and execution-order last-use rules are reused with stricter attachment identity, range and dependency
checks. Only compatible LOAD successors share a rendering scope; sampling an earlier attachment requires the
corresponding barrier outside rendering. The NRI backend has no discard load operation, so author DISCARD is
lowered to a clear, while discard stores remain native discard stores.

The `render.graph.cpu_recording` probe compiles against declared capabilities and destroys the graph definition
before replaying the executable's packets. It evaluates a fixture compute kernel and Buffer operations in software
through the same packet dispatch used by NRI. The native graph probes validate the generated work on GPU.

The top-level `LUX_BUILD_MODULES_ONLY=ON` option configures the reusable modules. Combine it with
`LUX_BUILD_MATERIAL_COMPILER=ON` and `LUX_RENDER_BUILD_PROBES=ON` to register Render's probes without enabling
other component tests. On Windows, `LUX_RENDER_GRAPH_ALLOCATION_PROBE=ON` adds the Vulkan and D3D12 measurements.
These count Render DLL calls to C++ `operator new` on the calling thread during Prepare and Record. GPU timings
bracket the graph command buffer on the same graphics queue. CPU submit timing includes the two timestamp commands.

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
Start resolves actual NRI memory requirements. Dedicated allocations, incompatible memory types and activation
inside a merged rendering scope remain separate. Aliased members share offset zero within one resource kind;
this is not a general mixed Buffer/Texture heap packer. No dependency or CPU wait is added to make reuse possible.

Each FIF slot owns its physical objects and Memory independently. Alias activation establishes a memory barrier
before the new object's layout/access transitions. Cleanup releases descriptors, then resources, then each Memory
exactly once through the existing Session cleanup scope. Plan diagnostics report logical-to-physical mappings;
instance resource diagnostics report actual Memory groups, requested bytes and placement rejection reasons.

SubmissionOperation advances an ordered, frozen submission window across Queue capacity limits. Each accepted batch records its
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

With `LUX_RENDER_DEVELOPMENT_METRICS=ON`, `RenderServer::developmentStats()` exposes cumulative Job selection,
input rebuild and CPU round occupancy counters. The round borrows structural Job relations, Target sources and
Resource identities until its submission terminates. Requests, cadence and completions only change the next
round's selection. Selection and topology growth counters track backing-array allocations in those two workspaces.
The preparation interval runs from scheduling program work to
its delivery on the Server. Slot hold time ends when the Server releases the round, before any remaining GPU
retirement. `dumpSubmissions` also reports imported overlap time and a histogram indexed by the number of
accesses to a physical resource. The default build disables this sampling.

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
Remove and Resize return busy while an execution is borrowed. Stop closes future admission while the admitted
execution settles. Professional physical release also returns
busy while such a window exists; it does not destroy objects referenced by future batches. Standalone callers must
finish or cancel that window before destroying its borrowed inputs. These checks end at CPU submission completion,
not GPU completion; physical cleanup still uses only DeviceSession.cleanup_scope_.

`declareHistory` creates a Previous/Current pair. `publishHistory` observes the complete resulting version,
including inherited mip ranges. Distance counts backwards over successful publications. Each bank retains
the latest samples within that distance and allocates FIF plus distance physical slots.
Image initial values are explicit; Buffer history currently
starts at zero and requires a multiple-of-four byte size. The instance owns F+1 physical slots, chooses a free write
slot by actual completion frontiers, and publishes only after the instance submission is accepted. A skipped writer
does not publish a sample. Descriptor views are created at Start, not on each rotation. Resize replaces only
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
Job conditions are zero/one bytes and are sampled once during preparation. Conditional writes require
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
ResourceUploader owns staging, upload batches and copy-acceptance callbacks. FrameProgress selects the next strict
FIF slot and owns its accepted frontiers. Structural Resource commands borrow a reusable physical replica and
return their accepted transfer frontiers to that slot. ResourceCreateContext supplies these device services to factories.
Device, Scene and View own their ResourceSets. ResourceRegistry keeps the sole declaration directory and a
non-owning generational index. Effect owns its logical graph. JobExecution owns the device plan and instance and
borrows the Target output. The Client settles uploads and stops consuming Jobs before deleting their resources.

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
Failed preparation or submission restores staged dirty ranges. Frame sources use `stageBuffer` to copy directly
into owned staging before the callback returns. Later writes mark pending ranges independently of the staged selection.
A fully prepared replacement waits for actual acceptance before publishing its binding. This can precede graph
recording, but never splits the replacement to compensate for insufficient staging. Descriptor publication and dirty
adoption stay on the control owner. The Uploader owns the copy command windows;
ResourceUploader owns its independent per-physical-queue upload command windows and StagingArena. Queue owns neither.
The host settles accepted GPU use before destroying either command owner; destruction checks this contract and never
hides an outstanding use with a wait.

The Server rebuilds its non-owning Storage preparation list when running Jobs or GraphInstance identities
changes. Project GPU consumers collect the actual `StorageFrameUse` borrows at those changes and supply their span
to the connected FrameOperation; each entry names its storage and `StorageBindingOps`. Preparation callbacks are synchronous:
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

The Server runs bounded IPC and ManualContext passes around GPU completion, retirement, ready Jobs and
uploads. When no work can advance, IPC, continuations and stop wake the same retained notification. The
default `gpu_wait_ns` of 1 ms bounds the idle deadline for polling native GPU completion. The ManualContext
budget counts adopted nodes. Each continuation is responsible for returning promptly.

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
DeviceEntry owns FrameProgress and recyclable workspace. The connected FrameOperation holds the Round and joins parallel
Record work before SubmissionOperation uses the native queue.

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

## Output identity and swapchain flights

The Server stores Offscreen and Swapchain Targets as values. Each Target reserves its writer before asynchronous
attachment compilation and releases that reservation on the same operation's return. An existing writer's detach
only releases its own identity. Target operations check the writer and in-flight reads before changing the binding.

Offscreen owns the image, its views and the physical last-use frontiers. Same-frame consumers use graph submission
dependencies. Consumers of a paused producer resolve the last JobOutput by Target generation, epoch, image index
and content revision. They wait for its writer, and their accepted reads join its reuse frontier. Accepting a write
invalidates that image's old revision. Publication requires all producing submissions to be accepted. Preparation-only
submissions and disabled passes leave the previous publication intact. Target image dependencies derive from the
actual consumer bindings when needed.

SwapchainTarget owns a single flight. Acquire and Present run on NativeExecution and return to the Server. A READY
image can be acquired before a FIF slot and kept through writer replacement. NOT_READY leaves the image index and
semaphore association unchanged. Queue acceptance of the acquire wait protects reuse of that acquire semaphore.
Each Present wait semaphore follows a swapchain image. In steady state the new submission waits for the image's
acquire signal before signaling that image's Present semaphore again.

Resize, Remove and Shutdown settle native flights and accepted GPU work before retiring a Swapchain Target. For
Vulkan devices with `swapchainMaintenance1`, retirement waits on the Present fences and releases unused acquired
images through the extension. On base Vulkan drivers, an empty submission waits on any unconsumed acquire
semaphore, then retirement waits on that device's Present queue. Both paths run on the owning NativeExecution
worker so that a wait on one device leaves the Server free to advance other devices.

The base Vulkan queue wait is an accepted driver compatibility boundary. It settles ordinary queue submissions,
including the empty acquire wait. Vulkan provides no portable guarantee that this wait proves the final unfenced
Present has released its synchronization objects. A clean validation run establishes the observed driver behavior
on the tested devices. [Khronos swapchain semaphore reuse guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)

Resize retires the current swapchain before creating another for the same window. An `oldSwapchain` handoff is
deferred. If creation fails after retirement, the Target reports its output as unavailable. The caller can retry
Resize on the same Target identity.

## Job control contracts

`JobCommands.hpp` exposes creation, start, stop, removal, dependency and cadence changes, exact execution
requests and Effect rebinding. `RenderServer` routes these commands to the Job and its current execution.

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

Use `cmake/render-v2-bootstrap` to configure Render and its source dependencies independently of Engine/Editor
products. Build `all -j 4 -- -k 0`, install to a fresh prefix, and use `cmake/installed-consumers/render-extension`
to check that prefix with SDK-only consumers. The bootstrap option `LUX_RENDER_BUILD_PROBES` adds the Render
and Material test targets, including GPU output readback. Build `editor/ui/render` separately against the installed
Render SDK and ImGui fork.

Engine/Editor product integration still needs to migrate old frame submission to `UpdateFrameData` and
`RequestRenderJob`, model attachment lifetimes as Jobs and Targets, route commands by Resource/Effect identity,
and use the independent UI extension for capture and snapshot submission. The old Engine material toolchain
requires a separate migration. Centralized transforms, cross-channel ordering for overlapping writes, capacity
shrinking and advanced history remain deferred.


## Deferred material composition

`compileMaterial(graph, purposes)` and the Cook entry points require explicit purposes. Forward supports Unlit;
GBuffer supports Unlit and metallic-roughness PBR; Shadow computes only AlphaMask coverage. Unrequested uses are not
compiled, and unsupported Forward lighting/Toon requests fail. Lighting and shadow-map sampling belong to features.
`MaterialProgramSet` owns the selected fragment uses, one reflected material record, parameter defaults and raster
state. The Material compiler emits Slang directly from author nodes and uses the shared shader compiler to
produce SPIR-V and DXIL with per-target reflection. Retain author MaterialSource assets and recook from them.
The retired v10 IR format cannot be read as a current material program. Preserve IR-only originals and use a
fixed older tool to export what can be recovered. The shared shader build inputs live in
`modules/function/material/shaders`. The Material compiler is a tool and test dependency.

If none of the requested uses reads a material record, the Cooked runtime parameter/texture lists are empty.
The author graph retains its slots; compiling another purpose can consume them later. Storage never invents
a layout for unused authoring data or creates a hidden Shader use to recover one.

The private GBuffer, deferred lighting and tonemap Features compose the built-in lit-scene capability.
Connect the GBuffer's `albedo_metallic`, `normal_roughness`, `emissive_ao` and `depth` ports to the lighting feature,
then its `hdr_color` to Tonemap. Port encoding identities supplement size/format matching: two equal-format images
with different channel meanings cannot be interchanged. GBuffer is linear RGBA8 + RGBA16F + RGBA16F and forward-Z D32.

GBuffer receives the draw groups and visibility streams from MeshVisibility. Position, UV, world normal and tangent
requirements come from the chosen Cooked fragment's stage inputs. Lit geometry supplies normals; normal maps also
require a tangent including handedness. Nonuniform and mirrored transforms use inverse-transpose normals and
orthogonalized linear-transform tangents. Double-sided state and explicit front winding belong to the group/material.
AlphaMask uses the same Cooked coverage in GBuffer and Shadow; transparent BLEND is rejected by this path.

GBuffer, main-view culling, lighting and shadow projection read the same View snapshot. `UpdateViewCamera` updates that
View through a semantic `CameraInput`, and Prepare applies the new revision. Tonemap reads exposure in EV from the
Effect. `makeDeferredRenderEffect` connects these Features for the four Shadow and Clustered combinations.

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

- ShadowMap contributes zero raster passes. Its GraphInstance data calculates explicit directional cascades, six point
  views or one spot view, plus dense light-to-slice ranges. It supplies `views`, `slices`, `light_ranges`, `raster_states`
  and the same `directional_lights`, `point_lights`, `spot_lights`, `light_counts` snapshot consumed by lighting.
- MeshVisibility reads `views` and the selected instance/geometry tables. Graph Reset/Cull/Compact passes produce
  the visible members and indexed indirect commands. Preparation checks the workload against device capacity.
- MeshShadow reads `views`, `raster_states` and the mesh draw input ports. It declares and writes `depth`.
  Shadow-purpose materials provide opaque/AlphaMask coverage.
- Set `DeferredLightingFeatureConfig::shadows` and connect the four light ports plus `shadow_views`, `shadow_slices`,
  `shadow_ranges` and `shadow_depth`. Its immutable comparison sampler is installed through the ordinary Job start path.
  Geometry groups and view counts must agree across the explicit connections; there is no sibling-Feature lookup.

`ShadowMapShape` selects equal-resolution 2D-array slices, one layer per view. It never reduces quality or drops lights
to fit a hidden atlas budget. Cascades use absolute camera-view distances; their last distance explicitly ends shadow
coverage. Projection reads the inverse view-projection and camera bounds from the frozen View snapshot.
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
without knowing shadows. Every GraphInstance/FIF slice has independent writable shadow images. Unchanged CPU projections
still redraw current depth; a cache hit is not a proof of valid GPU contents. Physical release uses the existing single
Session cleanup scope. Clustered lighting composes through the ports described below. EVSM, HZB occlusion,
deformation bounds and VSM/Nanite remain separate capabilities.

`render_deferred_scene` retains the CPU lighting reference. `render_shadow_scene` uses public Commands and independent
Feature registration, then test-owned Graph Copies to check actual GPU visibility, shadow pixels, projection data and
AlphaMask coverage. These output probes do not add a production Readback command.

## Clustered light composition

The private `ClusteredLightingFeature` contributes Reset/Build/Count/Scan/Fill graph work;
it does not call DeferredLighting, ShadowMap or GBuffer. `ClusterGrid` declares XYZ cells, linear/logarithmic depth
slices and an explicit index capacity. Build reads the finite forward-Z inverse projection, camera position and
near/far distances from the same View snapshot used by shading.

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
times finite lights, Scan to cells, and normal shading to the selected list. Reset clears the status buffer;
Fill writes the range determined by Scan. Specialized performance
and any larger-grid scan/range optimization require equivalent-workload evidence.

`render_clustered_scene` compares actual pre-Tonemap RGBA16F pixels, copies one coherent frame of cluster data for an
independent CPU geometric/list oracle, and tests the production Scan with overflowing counts. `render_shadow_scene 3
--clustered` adds the same HDR comparison to shadows, multiple materials and lights,
including an intentionally tiny list capacity. These checks use the full-light result as a correctness reference,
not as a hidden replacement for the successful non-overflow Clustered path.

## Scene composition and measurement boundaries

A draw group is keyed by Mesh, submesh and the material program's generational identity. Its instances keep
their own MaterialIds and GPU parameter addresses. The group publishes its live materials before visibility
compaction so preparation can resolve the parameters and current textures for every potential draw.

Connect MeshVisibility's group, instance and visibility ports to GBuffer. Setting `frustum_culling` to false keeps
the all-candidate reference path. With `camera_view` enabled, a Compute pass derives the geometric view and clip
planes from View. For shadows, the composition connects the projected `views` input. Each GraphInstance owns its
own FIF buffers.

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

Resource declarations and Storage implementations have separate identities. Each device's Device, Scene or View scope
owns at most one instance of each declaration. Two declarations can reuse one StorageMeta and factory
without sharing instance data. ResourceSet owns the instances; ResourceRegistry only indexes them.

Ordinary clients include `commands/ResourceCommands.hpp` and `resources/BuiltinResources.hpp`.
They query or explicitly create the desired Resource before uploading:

```cpp
auto resource = co_await client.request<GetOrCreateResource>(
    ResourceRequest{device_session, DeviceScope{}, kTextureResourceName}
);
if (resource.error.type != 0) co_return resource.error;
auto uploaded = co_await client.request<TextureCommands::Upload>(
    upload_command, TextureCommands::Upload::Request{resource.resource_id, data}
);
```

`QueryResource` never creates an instance. A Scene/View query can resolve a Resource in its actual
ancestor domain. Lookups of View resources require a View identity. Existing item IDs contain the
parent ResourceInstanceId and local generation. Upload/create requests use resource_id, while item
updates and releases use the complete item ID; neither repeats an Effect implementation or Scene route.
The request source remains borrowed through the terminal response. A failed execution candidate does
not delete a Resource that has already been published into its domain.

Creation configuration is registered with the Resource, independent of Effect parameters. A non-void creation config
must be installed through `registerResource` before a capability reuses that declaration. Query and GetOrCreate
share declaration/domain/device qualification; Query never constructs. Cold graph compilation resolves names to
ResourceTypeId requirements. Start reuses that same creation path and retains no extra Resource owner. A failed
attachment candidate does not remove already published resources. Preparation deduplicates ResourceInstanceId.

Different declarations can share the exact StorageMeta and Factory while owning separate instances and Command
associations. Repeated registration must agree on policy, creation defaults and methods. A resource command's
instance selects its declaration first, then its exact thunk; implementation type only qualifies the final borrow.
Stateless resource methods allocate no Handler object. Admitted asynchronous commands retain ResourceCommandUse
through terminal delivery; Scene/View removal returns Busy while such a borrow exists. Removal revokes identities
before destruction, and dense record moves repair the index. References are allowed only within the same actual
owning domain or its real ancestors on the same device.

JobExecution supplies the frozen instances selected for the current FrameOperation.

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

Shutdown closes admission and keeps Server delivery open through accepted command, frame, transfer and WSI returns.
It waits for both Contexts to become idle, including callback-stack and publisher tails, then settles the GPU
frontiers. The device domain releases Jobs, Targets, Resources and DeviceSession in dependency order. Target retirement
also settles the window borrow before `run()` returns. DeviceSession cleanup finishes before the NativeExecution worker
closes and joins. Context allocation capacity is a hint. Frame, Job, Uploader and Queue owners set admission limits.

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

Cooked fixed constant layouts record the matrix order and every enclosing fixed array's count and
stride for each leaf. The SPIR-V verifier checks these facts against native types, including Slang's
column-vector and array wrappers. Legacy blocks can still use structured layout checks. A typed
constant contract requires the additional target layout evidence, so legacy blocks require recooking
before such a contract can qualify.
The Windows cooker uses the pinned DXC reflection API to check each bound DXIL constant buffer's
register, field offsets, matrix storage and physical array strides. NRI layout creation checks the
root constant range against the device limit. The D3D12 budget includes the descriptor tables
formed by NRI from the final descriptor ranges.

D3D12 requires Shader Model 6.6 and loads the deployed Agility SDK beside the Render DLL.
It uses the existing contiguous instance-buffer path, with per-draw member offsets when
first-instance semantics are unavailable. Vulkan's optional physical-pointer paged variants
remain SPIR-V specializations; requesting buffer device addresses on D3D12 returns unsupported.
Both backends retain the same ordinary mesh, shadow, lighting and postprocess features.
This backend integration does not establish WSI, rendering-output or performance qualification.

The [Editor UI extension](../../../editor/ui/render/README.md) owns the ImGui dependency and its shaders.
Render provides the same generic indexed stream, staging and descriptor ownership to all extensions.

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

`resources/material/Resource.hpp` declares the Resource. `resource/material` owns immutable programs and shares
parameter pages among programs with equal reflected record ABIs. Its instances retain their own page and slot.
Programs with an empty record use the GPU sentinel and require no parameter buffer. The same Resource owns
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
`FrameData::appendData<LightResource, DirectLightUpdate>()`. Targets resolve during Job assembly.
Standalone `InstanceCommands::Update` and `LightingCommands::Update` use the same algorithms without an Effect.
Empty spans mean no update. Dynamic entries are checked before mutation; a multi-Resource packet is not a rollback
transaction. Mesh/material associations, regrouping observers and buffer-growth subscriptions remain
active. Clients end those references before releasing the referenced entries.

Active graph consumers share each Resource's real FIF slices. Structural changes rebuild a list deduplicated by
ResourceInstanceId; preparation tracks the source revision separately for each physical slot. Failed preparation
does not acknowledge dirty data. Accepted replacement buffers publish to all subscribers before subsequent use.
There is no Job-owned copy of the Scene transform table or second dirty/accepted owner.

Use `UpdateViewCamera` with a `ViewRequest` and `CameraInput`. Each View owns its camera and revision. Job assembly
and frame preparation project that View value into the selected shader regions. Effect configurations retain algorithm
parameters. Engine camera controls still need to adopt the View command.

UI rendering belongs to the standalone `editor/ui/render` extension. Register it explicitly with `registerUi()`;
its ordinary Feature recipe consumes a View-domain `UiResource`. `SubmitDrawData` copies a complete snapshot before
replying, including vertices, indices, draw order and image identities. Empty snapshots replace prior geometry.
Later executions of the UI Job reuse that snapshot; FrameData empty columns retain their no-update contract.
The Render core owns the generic indexed stream buffers, staging and native encoding, with no ImGui dependency.
Sampled Target identities participate in producer ordering, resize/removal and accepted GPU read lifetimes.

Camera and exposure have separate authorities: `UpdateViewCamera` updates View bytes; `SetEffectParameters` updates
shared Effect exposure in stops (negative values are valid). Feature recipes declare their parameter source, and
Instance resolves the selected layout once before copying changed source bytes during preparation.
`InstanceDataRecipe` belongs to the immutable Graph and is selected by Plan liveness; actual derived preparation
objects belong to GraphInstance and are destroyed in reverse construction order before its native resources.

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

Obtain the live layout with `QueryRenderJobInputLayout`, create two FrameData owners from it, and reuse
each one on its terminal reply. RenderClient admits at most two packet borrows per endpoint. Failed IPC
publication releases any local borrow before returning the request. A stale layout fails before applying data.
JobExecution holds the layout and its qualified bindings.

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

The active round borrows the device’s reusable Preparation workspace. Resource and Feature preparation receive it
alongside the Uploader, and register replica settlement with `Preparation::onSubmitted`. Uploader first commits/cancels its
staging ranges; the frame owner then delivers the native result through `finishCallbacks`. Callbacks appended
during delivery wait for a subsequent settlement. Reentrant settlement and destruction with pending callbacks
are ownership errors. The existing frozen ResourcePreparation API still settles its snapshots explicitly.

Uploader retains source-byte credits, staging reservations and transfer command windows. Independent Mesh and Texture
uploads progress during recording and when no Job is ready. Each accepted prefix keeps its queue frontier.
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
