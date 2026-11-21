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

### EffectGraph authoring

Pass an owned `EffectGraph` to `CreateEffect`. Register a `CapabilityDefinition` and an
`EffectImplementation` on the Server before running it. `EffectGraphEdit` connects versioned semantic ports
and publishes named outputs. Register node-targeted typed bindings with `registerNodeEffectCommand`, then list their
locations in the selected implementation. Each live node can initialize one typed state. Commands select that state by
Effect and NodeId before or after a Job starts.

`ListEffectCapabilities` returns self-contained definitions and implementation choices before a device exists.
The response owns its names and ports and can outlive the Server. Synchronous Catalog lookups borrow from
the Catalog, whose sealed definitions remain stable during graph validation and lowering.
Device and target qualification runs when the Job prepares its selected graph.

Hosts keep registered extension and receiver code loaded while Render can invoke it. At shutdown close
business admission, request stop, continue draining Client responses and completions, and join the Server thread.
After join, call `RenderClient::serverStopped` so real responses settle before any remaining requests. Destroy
the related messages, configurations, FrameData, operations, Client, and Server, and let their final callbacks
and typed destructors return before unloading extension code.

EffectGraph builds typed GPU work from the actual output format, sample count, Target kind, and device Shader ABI
during `StartRenderJob`. Target-relative extents are evaluated by the instance. Compatible starts share one in-flight
build and its terminal result. Live Jobs share the compiled plan and native program leases, with independent frame
resources and History.

Use `EffectBuildContext::importTargetImage` to declare a sampled input by name, format, and sample count. The Job
supplies the corresponding `TargetImageInput::binding` and sampled offscreen Target on the same device. The
compiler represents multiple names bound to the same Target as one resource. The variant key includes the
names, image contracts, and alias pattern. Different physical Targets with the same contracts and alias pattern can
reuse the plan. The bound instance uses the existing output publication and Resize contracts.

Use `EffectBuildContext::importResource` and `JobBinding::resources` to bind an existing same-device Resource
by name, `ResourceInstanceId` and a Resource-defined projection index. The index defaults to zero for a
Resource with one graph binding. `describeGraphImport` qualifies the shape, initialized ranges and boundary
use. `resolveGraphImport` supplies the FIF slices and maintains disjoint backing across distinct bindings,
including through backing replacement. A Resource publishes each changed physical slice on the device owner through
`ResourceRegistry::publishGraphInput(storage, projection, frame, input)`. The result counts the graph bindings
updated. Use `UINT32_MAX` to replace all slices, including clearing a projection before replacing its backing.
The registry indexes readers by Resource identity and projection. Each successful Job binding records its ticket;
failed Start, failed Rebind, replacement and Stop remove only that execution’s tickets before releasing its instance.
The first positional `StorageBindingOps::Resolve` signature is retained for the deferred Editor consumer.
Multiple names for the same owner and projection resolve to a single
graph resource. The Job retains each live owner through instance retirement. Variants can be reused with
different owners that supply the same projection contracts and alias pattern.

The built-in `Lighting` node consumes a GBuffer surface and an optional connected shadow bundle. Its
`direct` implementation binds the four `EDirectLightingBuffer` projections of a scene `LightResource`
and takes its View data from `JobBinding::view_parameters`. `SetLightingEnvironment` updates the node's
ambient radiance. Its HDR output connects to `ToneMapping`.
Select `Lighting::clustered` to build the bounds, count, scan and fill passes inside the node. The
default grid is 16 by 9 by 24, with logarithmic depth and capacity for 65,536 light indices. Override it
with named arguments such as `x=7 y=5 z=9 index-capacity=4096 depth=linear`. Depth accepts `linear` or
`logarithmic`. Both the list builder and the
deferred shader consume the same View snapshot. If a list overflows its capacity, shading evaluates the
complete local light list.

Connect `Lighting::cluster_lists` on the clustered implementation to consume the GPU buffers that the
lighting pass uses. The bundle carries grid metadata, cell bounds, counts, exclusive offsets, indices,
and status. `ClusterLists.hpp` defines the record layouts. Check status before consuming the indices.
Valid indices use bit 31 to select spot lights and the low 31 bits for the dense light index.

Use `declareCpuData<T>()` and `addPreparation()` to declare CPU-only producer dependencies in an
EffectGraph. A preparation declares its input values, unique outputs, associated GPU passes, and complete
typed inline block writers. The compiler validates the whole CPU dependency graph, then finds the live
GPU passes and CPU producers together. A live GPU pass retains its associated preparations, which retain their
declared passes and input producers. GPU execution order and synchronization follow the actual resource uses and
explicit graph dependencies. The compiler links typed configurations to final slots. Resolved inputs contain
final slots and owned values. Job creation uses the shared prototypes
to create independent producer objects without source graph or shader schema lookups.

During preparation, a producer reads published values with `FramePreparation::read`, stages outputs with
`publish`, and supplies complete inline constants with `write`. All declared outputs must be supplied on each
successful call. Values belong to the current Job preparation revision. Failure stops the chain before any
inline blocks are adopted into the writable frame. CPU-only slots are outside the GPU resource,
descriptor and synchronization tables.

Use `importStorageSnapshot<T>()` for CPU preparation data supplied by a Resource. Bind the named input with
`EJobResourceUse::CPU_SNAPSHOT`, declare it in `GraphPreparation::storage_inputs`, and resolve its typed slot
with `resolveStorageSnapshot<T>()`. The Resource registers a capture function for the owning value type.
Each Job retains its actual Resource owner and reuses its own snapshot storage. Selection finishes before the
producer callbacks, and the reference returned by `readStorage()` is valid for the current callback.
CPU snapshot inputs create no graph Buffer, GPU view, or barrier. Declare any GPU access to the same owner
through a GPU binding as well.

List any snapshot that controls runtime Raster programs in both `storage_inputs` and `program_storage_inputs`.
Its Storage must register a monotonic owner-side revision starting at one and return that revision from capture.
Read the value during `selectRasterPrograms()` with `RasterProgramSelection::readStorage()`.
The renderer compares the producer and Storage revisions before compiling, after asynchronous preparation
and before preparing a frame.
Changes discard stale candidates and trigger another selection. Mesh producers can resolve a snapshot's
`MaterialId` through the stream's named owner with `materialProgramId()`, then borrow its program artifact.

For Mesh preparation, bind the scene's `InstanceResource` as a `MeshInstanceSnapshot` to read instance,
mesh, material and submesh identities as well as render permission. The snapshot revision advances when membership,
these selections, or render permission changes. Built-in Mesh producers omit disabled instances from group and
program preparation. Transform edits leave the selection revision stable. Bind the GPU fields separately through
`importResource()`. Contiguous sessions use `EMeshInstanceBinding::PROPERTIES` and `ALIVE`, and
`ETransformGraphProjection::RECORDS`. Paged sessions use one `PAGED_ROOT` import for both instance
fields and `PAGE_ADDRESSES` for transforms. Storage publishes the current backing and tracks the pages
reached through device addresses. These projections grant read-only storage access.
The snapshot also carries each instance's material program identity and a deduplicated section table with
arena offsets and bounds. Geometry records are selected on the Server owner with the instance identities.

The built-in `MeshVisibility` camera implementation generates a `draws` bundle with the group table,
visible members, and indexed indirect commands. Bind the Job's `view_parameters` and use the named
`mesh.instances`, `mesh.properties`, `mesh.transforms`, and contiguous `mesh.alive` inputs.
The address-enabled implementation reads both instance fields through the `PAGED_ROOT` projection.
The node defaults to frustum culling. Pass `frustum-culling=false` as the node argument to keep every enabled instance
with a nonsingular transform. Each group has one command per winding. Consumers use the bundle inside
the same graph, where the compiler schedules the visibility writes before their reads.

Use `GBuffer::definition()` to connect the built-in indexed mesh surface to `Lighting::surface`.
The node runs camera visibility and consumes the same grouping in its RasterStream.
Choose `GBuffer::indexed` or `GBuffer::indexed_clockwise` for the authored front face and use
the same view and scene bindings as MeshVisibility. Bind `mesh.geometry` and `mesh.materials`
to the Job's Mesh and Material owners. Bind `mesh.textures` for programs that sample the shared texture table.
The surface bundle includes albedo/metallic, normal/roughness, emissive/ambient occlusion, and forward-Z depth.
Connect the optional `GBuffer::draws` output to consume the same visibility bundle as the raster pass.
When only this output is demanded, the implementation builds visibility without the raster pass or its material bindings.

Use `MeshRaster::definition()` for a single camera raster pass with the same Mesh and View bindings.
The forward implementations use cooked forward materials and publish an RGBA8 display color. The
depth-only implementations use the cooked shadow material path, including AlphaMask, and publish
forward-Z camera depth that can feed `DepthPyramid`.

Use `Shadow::definition()` to connect the indexed shadow caster path to `Lighting::shadows`.
Choose `Shadow::indexed` or `Shadow::indexed_clockwise` and supply the same Mesh, Material, Transform,
and View bindings as GBuffer. Bind `shadow.lights` to the Light resource's CPU snapshot. Optional
node arguments default to `resolution=1024 distance=100 pcf-radius=1`.
Pass `frustum-culling=false` to disable frustum culling for shadow casters.
The number of active cascades and light faces determines the depth array layers for each prepared frame.
The RasterStream applies each light's depth bias and publishes the layer and light mappings with the depth image.
The optional `Shadow::draws` port exposes the visibility groups, members, and indirect commands used by the caster
pass. The command table contains two winding streams per active view of each group.

Use `declarePreparedBuffer()` for read-only graph buffers filled by CPU producers. List the resource in
`GraphPreparation::gpu_buffers` and resolve its writer with `resolvePreparedBuffer()`. During preparation,
call `writeBuffer()` with the complete payload and an optional revision. A nonzero revision identifies the same
bytes across preparations of that Job. The graph retains the producer if a live pass reads the buffer.
Each FIF replica tracks its native allocation generation, accepted revision and size. A matching nonzero revision and
size select the resident binding directly. New selections copy the bytes into Job-owned storage before the callback
returns. An upload can span several windows. Only acceptance of its complete payload marks the revision resident.

Use `declareFrameBuffer()` when graph commands produce the buffer contents and the preparation determines its size.
List it in `GraphPreparation::gpu_buffers`, resolve a sizer with `resolveFrameBuffer()`, and call `sizeBuffer()`
once during each successful preparation. Sizes are multiples of four within the declared bounds.
Declare whole-buffer accesses and use the version produced by the GPU write for subsequent reads.
Each writable frame keeps its own growable allocation. Descriptors and indirect draw ranges are checked against
the selected logical size.

A node can give its producer a typed snapshot by initializing its state with
`EffectInitializationContext::initializeState` and resolving that type with
`InstanceLinkContext::resolveNodeState<T>()`. `FramePreparation::readNodeState` returns a value selected on
the Server owner before any producer runs. The linked slot belongs to the producer and follows the Effect's
lifetime. Command dispatch updates the logical state, and recording consumes the prepared inline bytes.

Use `ToneMapping::definition()` to author the built-in HDR-to-display node. Choose the `linear` or `srgb`
implementation, connect a `lux.color.linear-hdr` input to `hdr_color`, and consume the display value from
`color`. Pass an optional `exposure=1` in stops. Exposure defaults to 1.
`SetToneMappingExposure` addresses an Effect and a node, including before a Job is attached.

Built-in node arguments use named options from the installed `lux-cxx` arguments component
(`lux/cxx/arguments/TypedOptions.hpp`). The Server parses live nodes during `CreateEffect`. Extensions can
publish a typed immutable value with `EffectInitializationContext::setConfiguration` and borrow it in
a device build with `EffectBuildContext::configuration<T>()`. The value belongs to the logical Effect.

`effect/EffectBundles.hpp` defines the GBuffer, Shadow, HDR and cluster-list schemas shared by Effect producers
and consumers. Use the schema's type, version and member names when declaring or connecting these bundles.
Cold graph failures report an `EffectDiagnostic` through `RenderErrorSink` with the source node and pins where
available. The sink may retain a copy of this value after returning from the callback.

For a producer-authored Raster stream, declare the program choices and read-only resource envelope on
`RasterPassTemplate`. The producer's preparation declares ownership of that pass. During cold linking,
`resolveDrawStream` and `resolveRasterProgram` grant slots scoped to the compiled variant. Buffer, image,
sampler and descriptor argument lookups also happen during linking. Each live program uses its own reflected
layout and native program lease. Program preparation maps each argument and array element directly to its
descriptor use. Each writable FIF counts all private draw sets before growing its pool, then resolves each supplied
binding once and writes sets by program and draw ordinal. The producer publishes draws and borrowed argument arrays with
`setRasterStream`. Supply one binding for each declared descriptor array element. The writable frame validates
program identity, constant size, geometry, buffer bounds, image subresources and sampler comparison mode before
materializing native draw packets.

Set `RasterPassTemplate::runtime_stream_programs` for a stream whose programs are selected by its producer.
Advance the revision returned by the kernel’s optional `rasterProgramRevision()` when the selection changes. In `selectRasterPrograms()`,
transfer the shader artifacts and provide the complete ordered set with `setPrograms()`. Each program
names a `ShaderCandidateSlot` in that artifact list. Several programs can use one candidate.
Use `reusePrograms()` for a stream whose complete program keys and binding order still match. Producers
can adopt their associated records in `settleRasterPrograms()` after the complete selection succeeds.
A failed or obsolete selection leaves the previous set resident, and requests for a failed revision
complete with the preparation error.
Use `RasterProgramSelection::meshLayout()` for an owned copy of the selected mesh's vertex layout and
`materialProgram()` for the selected material's immutable program set. Both lookups use the stream's
named owners. The material pointer is borrowed during selection. The preparation operation registers
one reader per material program until completion, including when the producer combines shader stages.
Use `FramePreparation::rasterProgram()` to obtain a slot for a selection index during the current call.
Slots carry the published program-set revision, so a producer must resolve them again after replacement.

For shared sampled 2D textures, resolve a named `TextureResource` with
`EffectBuildContext::importRasterTextures()` and assign its graph ID to `RasterPassTemplate::shared_textures`.
Provide that owner through the Job's named bindings with `EJobResourceUse::RASTER_TEXTURES`. Runtime programs
can use the device's shared sampled-image and sampler arrays. Call `FramePreparation::sampleTexture()` for
each texture the program can access during that preparation and use the returned image and sampler indices.
The renderer validates the texture identity against the bound owner and tracks the selected images through
native submission.

For reflected material pages, declare a named `MaterialResource` with
`EffectBuildContext::importRasterMaterials()`, set `RasterPassTemplate::shared_materials` and bind the owner
with `EJobResourceUse::RASTER_MATERIALS`. Declare the read-only `uMats` storage-buffer argument and any
shared sampled texture and sampler arguments used by the programs. The renderer prepares the writable
frame's material replicas before the producer runs. Call `FramePreparation::selectMaterial()` with the
program slot for each material the program can read. Use the returned page and record indices when
building GPU inputs. The renderer checks the reflected record layout and records the selected page
bytes and material texture reads. Textured programs require the corresponding named texture owner.

For uploaded indexed geometry, declare a named `MeshResource` with `EffectBuildContext::importRasterMeshes()`
and attach the result to `RasterPassTemplate::mesh_geometry`. Bind the owner with
`EJobResourceUse::RASTER_MESHES`. Call `FramePreparation::selectMesh()` during each preparation and use the
returned vertex slice, index slice and index type. Mesh slices are scoped to the current stream and
preparation. The renderer checks the program's vertex layout, index width and slice bounds and tracks
the selected byte ranges through native submission. Indexed draws using selected Mesh vertices bind the
whole vertex slice and use the corresponding mesh's indices with zero base vertex. The producer keeps
GPU-written indirect arguments within those bounds.

For the mesh arena's shared-buffer offsets, set `RasterStreamIndexBinding::origin` to
`ERasterIndexOrigin::MESH_ARENA`. Use the selected mesh's `arena_first_index` and `arena_vertex_offset`
when generating its indexed command. Each authored draw supplies the complete vertex and index slices
for one mesh and one indirect record. Compatible records in the same rendering section can share a
native multi-draw call. Programs choosing this origin must accept native draw IDs within a merged call.
Dependency tracking retains each selected mesh's original byte ranges after merging.

The graph supplies attachment operations and viewport defaults. To record one rendering section per layer in a
fixed attachment range, pass a section table with one entry for each layer. Each entry selects a layer relative
to every color, depth and resolve attachment range and may override the normalized viewport. The draw slices
cover the submitted draws in order. A section with no draws still applies the graph's load and store operations.
With an empty table, the stream uses the original attachment scope. A runtime-layer attachment requires a
section table. Its length selects the physical layer count for the writable frame. Streams sharing an attachment
must agree on that count before any image is replaced. Direct vertex and indexed draws can use vertex
buffers, read-only private buffer bindings, sampled 2D or 2D-array views and complete per-draw inline blocks.
For independent depth bias per layer, declare `RasterPassTemplate::section_depth_bias` and attach a depth image.
The device must support dynamic depth bias. Supply finite constant and slope values in each section, including
an explicit zero when a layer needs no bias. The compiler enables the native state and the frame validates all
sections before replacing any attachment.
A stream can also submit GPU-written indirect commands with an optional count word. Declare both slices with
`EGraphUse::INDIRECT` and the optional index slice with `EGraphUse::INDEX`. A linear record holds four 32-bit
fields in the order vertex count, instance count, first vertex and first instance. An indexed record holds five
32-bit fields in the order index count, instance count, first index, signed vertex offset and first instance.
The graph producer supplies command values within the declared geometry slices and device limits.
The graph declares the readable mip and layer ranges and the sampler choices. Graph access declarations determine
the synchronization for those reads. Replacing a physical image retires its old Raster views with the existing
resource lifetime rules. An empty stream still executes the authored attachment operations.

### Resource ownership and composition

Resource upload is independent of Effect creation. `GetOrCreateResource` selects the owner in the
device, Scene or View domain. Resource commands route through the full owner or item identity.
An EffectGraph names the resources it uses. At Job assembly the Server qualifies each supplied owner,
its binding projection and the resource contract before creating an execution.

`EffectCatalog` owns capability definitions and implementations. A capability defines semantic ports
and schema versions. Select an implementation and connect the ports in `EffectGraphEdit`. For a deferred
scene, connect the `GBuffer` surface to `Lighting` and its HDR result to `ToneMapping`. Connect the
`Shadow` output when needed. The clustered lighting implementation also exposes its list buffers as a
separate output.

Register Resources, capability definitions, implementations and command bindings before `RenderServer::run()`
seals the catalog. Resource registration owns the corresponding command and FrameData types. Node commands
route to state initialized by the selected implementation. Plugin implementations retain a code lease for their
callbacks and state.

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

`JobExecution` owns CPU input bindings and Resource subscriptions. The device variant owns each producer's immutable linked inputs.
Register a kernel with `ProducerFactory::make<Kernel>()`. Its static `resolve` links configuration once,
and `prepare(const LinkedData&, FramePreparation&, Workspace&)` receives the current Job's workspace.
The instance owns each workspace and the per-FIF GPU bindings. A kernel may supply a fallible
`createWorkspace` when construction depends on linked inputs or the target. Cold assembly
resolves Resource borrows once; instance preparation uses those borrows and contributes copies to `Preparation`.
After Target prefetch, FrameProgress opens the next strict FIF slice for the ready Jobs. FrameOperation prepares
Resources and instance data and submits preparation copies before graph work. Job teardown releases its workspaces before releasing the device variant. Physical cleanup uses the Session scope.

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
   to `QueryRenderDevice`. The Server can hold multiple Sessions for device queries.
   `CreateTarget`, `QueryResource` and `GetOrCreateResource` qualify the device explicitly. Scene and View IDs
   describe logical scopes. Each device owns its own Resource instances for those scopes; use `DeviceScope`
   for a device-level declaration. Asset commands keep routing from their parent ResourceInstanceId.
   `RemoveDeviceSession` returns Busy while the device has a Target, execution or active borrow. Once
   admitted, removal closes that device to new work and replies after its accepted GPU work and resources
   have retired. The other devices continue to submit while removal is in flight.
   Upload meshes and textures before any Job exists, and await their typed replies before using the resulting IDs.
3. Create the Scene, View and Target, then pass an `EffectGraph` to `CreateEffect`. Create the Job
   with the Scene, View and Effect IDs. Pass the device and its Target to `StartRenderJob` to prepare execution.
   A running Rebind stays on that device. Cross-device Target reads and execution dependencies are rejected.
   Each device owns its frame slots, upload state and published output history. The Server visits ready devices
   in rotating order. QueryRenderDevice reports a device's execution error separately from the query status.
   A terminal device error closes its admission while healthy devices continue submitting.
   Use `JobBinding::target_images` to associate a declared input name with a sampled producer Target.
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

## Graph compilation and recording

The Builder owns graph-local resource versions and strongly typed Raster, Compute and Transfer commands.
Extensions declare their attachment scopes, resource ranges and usage through those commands. The compiler validates
each command and lowers it into a recording block referencing a typed packet table.

Compute and Transfer compilation resolves resource versions into typed image or buffer operands. Freeze selects the
current History bank or fallback, checks the bound slice, and resolves indirect offsets, dispatch dimensions,
compressed block regions and copy pitches. Recording consumes those per-FIF native operands. Resize validates
candidate image extents before adoption and the next Freeze resolves the new physical bindings. Dynamic producers link `RasterStream` slots during
compilation and supply draws and resource bindings through `FramePreparation`. The graph declares attachments
and fixed graph resources.

`RenderGraphBuilder` indexes each program’s reflected fields when the program is added. Raster, Compute and Transfer
each normalize their own work into a private draft. The Builder publishes passes and write versions after range,
binding and overlap validation. A Compute sequence publishes its passes and dependencies together.
`finish()` produces one self-contained immutable Definition;
`analyzeRenderGraph()` produces independent analysis data. Graph resource/pass IDs are indices within that Definition, not
global generational identities. Version zero describes explicit initial contents; positive versions name writes.
Reads, read/write predecessors, conditional fallbacks and overlapping ranges are explicit. Exports, published History,
imported writes and explicit side effects root culling. Invalid ranges, live cycles, ambiguous producers and incomplete
reads fail with structured errors.
Untouched subranges inherit the explicit predecessor; analysis follows those ranges only when a consumer observes
them. Logical lifetime disjointness does not authorize physical memory aliasing.

A normalized `GraphPass` owns its typed command. Inspection uses `kind()` and the borrowed `bindings()` view.
Fixed raster and compute commands own their shader interface. A dynamic raster command owns its parameter
block, and a transfer command owns the typed writes derived from its fills.

`addRasterPass`, `addComputePass` and `addTransferPass` derive accesses from the work's actual inputs,
attachments and operations. They return the new written versions; authors do not repeat the same accesses
in a separate Program callback. Raster attachment load/store operations are explicit. A partial texture copy
preserves its predecessor; it cannot manufacture initialized contents outside the copied rectangle.

EffectGraph validation checks node versions, semantic edge compatibility and output names. Lowering invokes
the selected implementations in topological order. Only live nodes and demanded outputs produce GPU work.
The RenderGraph compiler then resolves resource versions and liveness for GPU passes and CPU producers.

Use `nextWriteVersion` and `addComputeSequence` to declare a read of a preceding dispatch within one logical
Compute pass. The Builder commits the sequence as a unit and returns the final written version of each resource.
Analysis keeps the commands together, derives their hazards individually, and rejects dependency cycles. The compiler
records compatible adjacent work in the same CB and emits the synchronization between commands.

Compute work can declare permission to run asynchronously. The compiler uses a Graphics schedule by default.
A device can supply measured submit and queue handoff costs in DeviceOptions::graph_queue_costs, and graph authors can
supply workload-specific command timings in GraphPassOptions::estimated_gpu_ns. With complete timings for the live
graph, the compiler compares the Graphics and eligible async schedules using their actual compiled submission DAGs.
It estimates GPU completion across queues, includes native submit costs, and requires the winning async candidate to
preserve the baseline's reuse of identical physical objects. Runtime memory alias placement still uses NRI requirements.
Imported images, surface output uses, sequences, and conditional intervals constrain queue assignment.

Each Effect lowering invocation owns its builder, canonical import slots and bundle storage. Each node receives
its own pin range; the upstream input lists are valid through its build call. Producer linking assigns dense slots
once in live order and tracks stream claims for the entire link. Each producer publishes its linked payload and
operand lists after its factory and declaration checks succeed.

Each compile invocation owns the intermediate shader bindings, usage stages, pass-to-batch mapping and diagnostics.
The compile worker captures the device limits before planning. Recording-only compilation supplies declared device
facts to the same planner.

`GraphCompiler::compile(session, cpu, {graph, analysis}, options)` prepares device-qualified programs, descriptor bindings,
submissions and physical resource layout before publishing the `RenderGraphExecutable`. Shader reflection supplies
layouts and descriptor writes on SPIR-V and DXIL. Fixed descriptor operands and initial parameter bytes are stored
in contiguous tables indexed by pass ranges. Descriptor pool counts and instance parameter copies are computed
during compilation. Separate image and sampler bindings remain explicit. Program preparation returns to the owner
scheduler for publication through the existing `CompiledPrograms` cache. The device execution owns those native program
leases alongside the executable. The executable's recording tables carry slots and the Instance freezes their physical
bindings for the recording workers. Barrier predecessors and History predicates use compiled slots. The diagnostic
sidecar carries source Pass IDs, resource names and reuse reasons.

The Effect owns its logical graph. Each DeviceEntry owns the compiled variants for its device, keyed by Effect,
Target contract and qualified imports. A matching Start joins an in-flight compilation or reuses the ready plan.
The device removes a variant when its final Start or Job reader returns. Shared uniform buffers stay in the device's
Effect pool until the Effect or device retires.
The compiler links producers and constant writers to final slots. Job assembly borrows the plan and creates its own
producer instances, writable frame bindings, transient resources and History.
Standalone project GPU consumers can bind physical inputs directly.

The `RenderGraphExecutable` contains a recording program with typed Raster, Compute and Transfer packet tables,
range transitions and rendering groups. Each live pass stores one typed command slot. The Raster slot also
selects the per-pass scope, including its viewport and depth bias. Merged groups carry the first load and
last store across their member passes. Compute and Transfer allocate no raster scope.
The compiler checks dense packet order, scope counts and group membership before publishing the plan.
Legacy's consecutive-pass
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

`declareHistory(key, HistoryDeclaration{...})` declares the resource shape, first-frame value, canonical use and
backwards distance together and creates a Previous/Current pair. `publishHistory` observes the complete resulting version,
including inherited mip ranges. Distance counts backwards over successful publications. Each bank retains
the latest samples within that distance and allocates FIF plus distance physical slots.
Image initial values are explicit; Buffer history currently
starts at zero and requires a multiple-of-four byte size. The instance chooses a free write
slot by actual completion frontiers, and publishes only after the instance submission is accepted. A skipped writer
does not publish a sample. Descriptor views are created at Start, not on each rotation. Resize replaces only
History banks whose physical extent changed; other banks retain their allocation and invalidate their samples.
`InvalidateHistory` invalidates the selected attachment histories at the next preparation boundary.
An individual Previous read can name a compatible non-History resource as its first-sample fallback.
Analysis retains both inputs and their hazards; Prepare selects existing views or copy sources without copying
the fallback into a History slot. Different reads can declare different initial inputs. Fallback views are cached
with their frame bindings and change only when their physical input changes.

Resize keeps the `RenderGraphInstance`, its Storage subscriptions, producer bindings, descriptor pools and immutable
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

The `DepthPyramid` EffectGraph node consumes single-layer D32 forward-Z depth and produces an R32 depth pyramid.
Its `mips` argument defaults to one. Connect the `previous` output to request History with a
first-frame far-depth value of 1. The current-only path uses a transient image. Each compute dispatch reduces
the complete footprint of its mip, including odd dimensions. `ImageDispatchArguments` resolves group counts
from the actual mip extent, including after Resize.

`dumpGraph` and the project-side `dumpPlan` synchronously emit JSON or DOT into a borrowed sink.
They do not open files, wait for the GPU or trace stable frames. Compiled plan counts are distinct from measured
execution counters.

## Resources and frame lifetime

DeviceSession contains the device, immutable capability facts, unique physical Queue collection, shared
DescriptorTable and `cleanup_scope_`. The selected backend's extension table is private to its implementation.
ResourceUploader owns staging, upload batches and copy-acceptance callbacks. FrameProgress selects the next strict
FIF slot and owns its accepted frontiers. The command dispatcher supplies a structural writer for one admitted
Resource command. It records accepted transfers against the selected slot. Factories receive the device,
uploader availability, replica count and resource registry through ResourceCreateContext.
Device, Scene and View own their ResourceSets. ResourceRegistry keeps the sole declaration directory and a
non-owning generational index. Effect owns its logical graph. JobExecution borrows the device plan and Target
output and owns its graph instance. The Client settles uploads and stops consuming Jobs before deleting their resources.

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
An entry without that explicit qualification retains per-frame preparation. Producer-derived data can likewise
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
without consuming an index or selecting another slot. FrameProgress owns the CPU access mode and epoch. A frame or a
structural Resource writer holds the selected slot and epoch as values. Structural writes adopt their copy and acquire
frontiers into the selected slot without advancing the graphics frame index. A frame adopts each accepted submission
before releasing CPU access, and accepted prefixes remain in the slot after failure.

## Output identity and swapchain flights

The Server stores Offscreen and Swapchain Targets as values. Each Target reserves its writer before asynchronous
attachment compilation and releases that reservation on the same operation's return. An existing writer's detach
only releases its own identity. Target operations check the writer and in-flight reads before changing the binding.

Offscreen owns the image, its views and the physical last-use frontiers. The device’s structural input table
assigns one slot to each sampled Target and maps each consumer’s image imports to those slots. Admission resolves
each selected Target once. Acquire checks the selected publication’s epoch and readability, and accepted consumers
extend the frontier of that exact image slot. Same-frame consumers use graph submission dependencies. Consumers of a paused producer resolve the last JobOutput by Target generation, epoch, image index
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

Connect the `GBuffer::surface` bundle to `Lighting`, then connect the HDR output to `ToneMapping`.
The bundle carries albedo/metallic, normal/roughness, emissive/AO and forward-Z depth. GBuffer uses linear
RGBA8, two RGBA16F attachments and D32. Effect port semantics distinguish resources that share a format.

GBuffer receives the draw groups and visibility streams from MeshVisibility. Position, UV, world normal and tangent
requirements come from the chosen Cooked fragment's stage inputs. Lit geometry supplies normals; normal maps also
require a tangent including handedness. Nonuniform and mirrored transforms use inverse-transpose normals and
orthogonalized linear-transform tangents. Double-sided state and explicit front winding belong to the group/material.
AlphaMask uses the same Cooked coverage in GBuffer and Shadow; transparent BLEND is rejected by this path.

GBuffer, main-view culling, lighting and shadow projection read the same View snapshot. The View Resource supplies
the block and its revision. Connect GBuffer and the optional Shadow node to the selected Lighting implementation,
then connect its HDR output to ToneMapping. The ToneMapping node owns exposure in EV.

`LightResource` supplies Scene-local directional, finite-range point and spot arrays. Client Commands
borrow unique input records through Response and return stable LightIds. Dense GPU arrays keep a GPU-format replica
source and dirty ranges for the actual FIF slices; removal updates the moved record's index. Upload and replica repair
use ResourceUploader. Unchanged frames do not create light Senders or scan all light records on CPU.

DeferredLighting remains the all-lights reference for Unlit and metallic-roughness PBR. With no shadow configuration
it compiles the unshadowed path, with no dummy map or sampler. It has no hidden ambient term; AO affects only the
explicitly supplied environment term. Tonemap applies ACES and an explicit linear/sRGB output choice.

## Shadow composition

The `Shadow` implementation selects directional cascades, six point views or one spot view from the same light
snapshot consumed by `Lighting`. Its producer publishes projection and raster state, then Graph Reset/Cull/Compact
passes produce the visible members and indexed indirect commands. The RasterStream writes layered depth using
shadow-purpose material programs. The `shadow` output connects the depth and light-to-slice data to `Lighting`.
A separate optional draw output provides the visibility results.

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

The Deferred and Shadow GPU fixtures build their scenes with the installed EffectGraph interfaces. Test-owned probes
read back visibility, depth, lighting and projection data, including AlphaMask coverage.

## Clustered light composition

The clustered `Lighting` implementation builds Reset, Bounds, Count, Scan and Fill passes. `ClusterGrid`
describes the cell counts, linear or logarithmic depth and index capacity. List construction and shading
use the same frozen View block and light buffers. Bounds unprojects each cell's eight corners for a conservative
world-space AABB in both perspective and orthographic projections. Count and Fill share a sphere/AABB test.
Spotlights use their enclosing range spheres. Directional lights remain in a global loop. A single writer per cell
preserves dense point-then-spot order. Scan uses a 128-lane chunked Blelloch scan with saturating addition.

Connect `Lighting::cluster_lists` to consume the resulting buffers. Check the status before indexing the list.
Capacity overflow, arithmetic saturation and invalid grid projection set GPU status. Lighting evaluates the full
local light arrays in those cases and for pixels outside the grid, within the same frame.

Count and Fill scale with cells times finite lights, Scan with cells, and normal shading with the selected list.
The graph rebuilds the transient bounds and lists each execution and owns their synchronization and FIF lifetime.
The clustered fixture compares pre-ToneMapping RGBA16F pixels and reads the list against an independent CPU oracle.
It also exercises Scan with overflowing counts. The Shadow fixture repeats the HDR comparison with shadows and
multiple materials and exercises a deliberately constrained list capacity.

## Scene composition and measurement boundaries

A draw group is keyed by Mesh, submesh and the material program's generational identity. Its instances keep
their own MaterialIds and GPU parameter addresses. The group publishes its live materials before visibility
compaction so preparation can resolve the parameters and current textures for every potential draw.

Connect MeshVisibility's group, instance and visibility ports to GBuffer. Setting `frustum_culling` to false keeps
the all-candidate reference path. With `camera_view` enabled, a Compute pass derives the geometric view and clip
planes from View. For shadows, the composition connects the projected `views` input. Each GraphInstance owns its
own FIF buffers.

Visible members, commands and counts contain two streams per view: positive transform determinant, then negative.
Raster draw streams use the corresponding front-face state, including for double-sided material normal orientation.
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
owns at most one instance of each declaration. Two declarations can reuse one typed factory implementation and
register independent metadata. ResourceSet owns instances. ResourceRegistry owns registration metadata and indexes instances.

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

Registration imports the Resource definition, command bindings and frame channels into stable Registry storage.
The typed factory copies operation tables when it constructs the registration. The host keeps the callable code
loaded until the Server and its outstanding operations have been destroyed. A resource command's
instance selects its declaration first, then its exact thunk; implementation type only qualifies the final borrow.
Stateless resource methods allocate no Handler object. Command admission registers an active Resource use.
The terminal continuation ends that use on the Server before queuing the response. Scene/View removal returns Busy
while an active use exists. Removal revokes identities
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
all bindings. Each FIF keeps its own content cursor. Allocation, publication and release append slot identities
to a bounded 256-change journal. On the next writable use, adjacent live slots are copied together from the
current owner table. A cursor gap requires a full live copy. Table growth rebuilds that frame's pool
and invalidates affected recordings. Vulkan frames retain the shared native pool generations selected by their
frozen replicas until submission receipts have been adopted. A replaced pool returns its budget after destruction.
DXIL never passes through SPIR-V relocation. Runtime validation uses pinned SPIRV-Tools
and the resolved SPIRV-Cross package; generation has no Engine toolchain or PATH/SDK fallback.

`ResourceParameter::setUniformConstant<T>(offset)` binds exactly one read-only uniform buffer block to the
specified graph buffer range. The compiler checks the complete `ConstantBlockTraits<T>` against the selected
target's reflected layout and the device's uniform buffer offset and range limits. The backend validates the
physical buffer and applies native CBV padding as needed.

An Effect node can own an `EffectUniformBlock<T>` in its typed state. The node command sets its
value and advances the revision. During device build, `EffectBuildContext::uniformBlock<T, State, Member>(name)`
declares a buffer version for `setUniformConstant<T>()`. Each device round selects one revision of that
block for its Jobs. The version pool stages each revision once and keeps its backing until the upload,
prepared consumers, and accepted uses on every physical queue retire. The pool holds up to `FIF + 1`
versions per block. GPU completion advances the round when all backings are in use.

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
Render extensions link RasterStream slots and use prepared graph buffers for staging, with frame-owned descriptors.

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

Scene and View identities are logical scopes shared by Jobs on multiple Devices. Each Device owns its Resource
instances for those scopes. A Scene or View removal checks Jobs and active Resource use across all of its Devices.

Transform, Mesh instances and direct lights are Scene-owned Resources. Use their typed FrameData columns
`TransformRecordUpdate`, `MeshInstanceUpdate` and `DirectLightUpdate`. Targets resolve during Job assembly.
Standalone Resource commands invoke the same typed updates without an Effect.
Empty spans mean no update. Dynamic entries are checked before mutation; a multi-Resource packet is not a rollback
transaction. Mesh/material associations, regrouping observers and buffer-growth subscriptions remain
active. Clients end those references before releasing the referenced entries.

Active graph consumers share each Resource's real FIF slices. Structural changes rebuild a list deduplicated by
ResourceInstanceId; preparation tracks the source revision separately for each physical slot. Failed preparation
does not acknowledge dirty data. Accepted replacement buffers publish to all subscribers before subsequent use.
Create Scene transform records with `TransformCommands::Create` and set `MeshInstanceData::transform_id` to share them
with other Mesh instances in the same device and Scene. `TransformCommands::Update` and
`FrameData::appendData<TransformResource, TransformRecordUpdate>()` validate the whole batch before writing.
Rebinding or removing a Mesh instance releases its old reference. Releasing a referenced transform returns Busy.
TransformResource owns flat buffers or device-address pages per FIF replica. Mesh properties carry a qualified
Transform GPU slot, and culling, raster and shadow shaders load the matrix from that binding. Staging detaches its
selected dirty ranges, so later writes remain pending. Logical release invalidates the identity immediately.
The GPU slot becomes available after the active round settles and the accepted physical queue frontiers complete.

Update a material's reflected float4 color parameter through `MaterialCommands::UpdateColor` or
`FrameData::appendData<MaterialResource, MaterialColorUpdate>()`. Select the parameter index from the cooked program's
projection. The batch qualifies every record and field before adopting any values. The existing material page
supplies each FIF's upload, and the full material update command handles texture and structural changes.

To supply a View block, create `kViewParameterResourceName` with `GetOrCreateResource` in the Job's device and
`ViewRequest` scope. Set `JobBinding::view_parameters` to that Resource identity. The application supplies complete
`ViewGpuParameters`, including column-major matrices, and publishes through `ViewParameterCommands::Update` or the
`ViewParameterResource` FrameData column. Each batch validates every value before applying the last element.
At round admission, the Job selects the value and revision. Updates after that selection take effect in subsequent rounds.
Producers read the selected values through `FramePreparation::viewParameters()` and write complete shader
constant blocks. `SetToneMappingExposure` addresses a ToneMapping node by EffectId and NodeId. Compilation
validates the block's reflected ABI and links its writer. Before Freeze, a changed exposure revision updates the
writable FIF replica and invalidates the recording that references that block.

## CPU input columns

`FrameLayout` owns the channel descriptions identified by Resource declaration and element type.
`makeFrameLayout(std::type_identity<std::tuple<Resources...>>{}, revision)` creates a standalone typed schema.
`QueryRenderJobInputLayout` returns an independent copy with a Server-issued `FrameLayoutKey`. Admission checks the
Server instance and input revision. A Rebind with the same Target, channels and Resource identities retains the key.

`FrameData::create<Resources>(layout, initial_capacities)` checks the complete typed contract and fixes its
column directory. Each column owns one typed vector. `appendData<Resource, T>(count)` grows that column and
returns only newly constructed elements; `addData<Resource>(value)` appends one. `getData<Resource, T>()`
returns only valid const elements. Fill an appended span before sealing; growth of the same column invalidates
its old spans. Other columns and another CPU slot can grow independently. Reset retains capacity; an empty
column means no update, and cross-column ordering does not express last-writer-wins semantics.

`seal()` makes the owner immutable. Writes/reservations reject a sealed packet; reset, move and destruction
are fatal ownership violations. RenderClient moves the packet from sealed to in flight at operation start, then
restores it to writable before terminal delivery on the caller thread. A second submission of the same slot fails.
Sending or cancelling an accepted request does not return it.
Raw IPC callers must retain the sealed owner and call `release()` only after the actual response or host settlement.
Element metadata and code remain loaded until the packet owner is destroyed.

Obtain the live layout with `QueryRenderJobInputLayout`, create two FrameData owners from it, and reuse
each one on its terminal reply. RenderClient admits at most two packet borrows per endpoint. Failed IPC
publication releases any local borrow before returning the request. A stale layout fails before applying data.
JobExecution holds the layout and its qualified bindings. The Server adopts `UpdateFrameData` during dispatch.
For an EffectGraph Job, the layout includes the retained Resources with update channels in the Job's
Device, Scene, or View scope. Repeated bindings of the same Resource contribute one batch.
A packet-bearing `RequestRenderJob` first takes the Job's unique request credit, then adopts the packet during
the same dispatch. The acknowledgement for a render request follows native submission acceptance.
Nonempty Resource batches are prechecked for exclusive commands before any write. Each Resource validates its
own batch before applying it. Mesh instance updates change references, and Transform updates change matrix values.

## Shared copy recording and preparation settlement

Graph transfers, staged asset/table uploads, texture clones and producer buffer preservation use the physical
recording functions in `extensions/device/CopyEncoding.hpp`. Callers supply the exclusive command buffer and
qualified resource ranges. The shared texture encoder expands mip/layer regions; upload/readback retain the
qualified native row and slice layout. Graph compilation still owns graph barriers; Uploader owns its transfer
and queue-family handoffs. The encoders neither allocate resources nor submit or wait for GPU work.

`BufferCopyRegion` is a physical copy value independent of ResourceUploader. Producer preservation is recorded
by the frame owner through `recordPreservingCopies`, including its existing source/destination transitions.
Uploader continues to own staging pages and transfer command windows; GraphInstance owns graph command resources.
Both submit through the existing SubmissionQueue and NativeExecution with the same accepted/rejected/unknown facts.

The active frame tracks Resource participants by instance ID and Producer uploads by Job generation and linked
buffer slot. Registered Resource operations receive a slot and epoch at preparation. Each attempted participant
receives a settlement, including zero-copy attempts and attempts that staged a prefix before failing. Return
false from preparation when publishing a binding during settlement is required before its next use.

Each accepted frame-copy receipt names the physical queue, serial and staged range count. FrameProgress adopts
that point before staging and participants settle. Resource revisions become reusable only after successful
settlement. A failed frame copy restores staged dirtiness while the Resource retains its authoritative values.

Uploader retains source-byte credits, staging reservations and transfer command windows. Buffer writes, image uploads
and clone patches each qualify their source layout before building copy chunks for the common submission path.
Extensions use writeImage for uploads and cloneImage for patches. Mesh and Texture uploads progress during recording
and when no Job is ready. Each accepted prefix keeps its queue frontier.
CPU sources end at their operation terminal, and staging pages wait for their GPU completion callbacks.

## Shader and frame preparation

`GraphProgram` retains SPIR-V and DXIL artifacts with their own reflection. Logical construction verifies that
both targets expose the same named resource/access contract; it does not equate their physical offsets or registers.
Device compilation selects the target and resolves binding names against that target's reflection. Embedded shaders
are already compiled build outputs, so loading both artifacts does not invoke a compiler during construction.
Implementations select device-dependent values, such as indirect-first-instance support, when declaring their
complete constant blocks. The compiler checks those blocks against the selected target's reflection.
Explicit `zero_fill_tail` constant bindings initialize only the supplied byte prefix and zero the selected target's
remaining reflected bytes; ordinary exact-size bindings remain strict. UI and fullscreen passes use this for
target-dependent constant-block padding.

Producers write complete parameter blocks into the writable FIF slice. `prepareFrame` freezes its physical
bindings, pass inputs and History initialization commands for `FrameRecordInput`. `FrameExecutor` borrows those
stable slices during synchronous recording; it has no GraphInstance access and performs no texture-description
queries. Changing author parameter bytes after preparation affects the next preparation, not this recording.
Physical binding or recording invalidation requires preparation again. The caller retains exclusive access to
the FIF data through recording and waits for accepted GPU uses before reusing its command allocators.

### Structural Resource commands

An asynchronous Resource command declares `requires_idle_resource = true` and registers a handler taking
`Storage&, ReplicaMutation&, Request&&`. After validating its input, the handler awaits `mutation.select()` on the
device owner. The result gives the selected frame slot and epoch. Repeated selection in the same command reuses
that slot. Await `mutation.writeBuffers(writes)` to attach each accepted copy to the slot's physical queue frontier.
Adopt the candidate only on success and use the device retirement path for an already referenced failed candidate.
The dispatch operation ends CPU access on terminal completion. The mutation reference and its writes belong to
that call. Regular preparation still uses its frame-scoped stage and settle callbacks.

### C++ parameter limit

Render functions, constructors, factories, lambdas and callback contracts support at most four explicit parameters.
Use the domain value or the actual owner at the call site. The first StorageBindingOps resolve slot retains its
Editor consumer signature until their joint migration.

From the repository root, run the source signature check with Python and the pinned grammar packages:

```sh
python -m pip install tree-sitter==0.26.1 tree-sitter-cpp==0.23.4
python modules/function/render/test/CheckSignatureArity.py --base <commit-before-change>
```

The check compares tracked changes and new source files with the supplied commit. It scans source-spelled
parameter lists, including constructors, lambdas and function pointer types. Compile template and macro consumers
in the modules and installed SDK builds as well. The default base is HEAD for local uncommitted changes.
