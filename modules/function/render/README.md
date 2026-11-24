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

Pass an owned `EffectGraph` to `CreateEffect`. Register an `EffectCapability` and an
`ImplementationEntry` on the Server before running it. `EffectGraphEdit` connects versioned semantic ports
and publishes named outputs. Register node-targeted typed bindings with `registerNodeEffectCommand`, then list their
locations in the selected implementation. Each live node can initialize one typed state. Commands select that state by
Effect and NodeId before or after a Job starts.

An author can declare fixed ports and requirements in static arrays and register an `inline constexpr EffectCapability`.
For runtime declarations all names and arrays need only stay valid through `registerEffectCapability`.
The Catalog copies the names, port types and resource requirements synchronously. Graph nodes own their selected
names. Built-in declarations are available from `effect/capabilities/` and their bundles from `effect/resources/`.

`ListEffectCapabilities` returns self-contained definitions and implementation choices before a device exists.
The response owns its names and ports and can outlive the Server. Synchronous Catalog lookups borrow from
the Catalog, whose sealed definitions remain stable during graph validation and lowering.
Device and target qualification runs when the Job prepares its selected graph.

Hosts keep registered extension and receiver code loaded while Render can invoke it. At shutdown close
business admission, request stop, continue draining Client responses and completions, and join the Server thread.
After join, call `RenderClient::serverStopped` so real responses settle before any remaining requests. Destroy
the related messages, configurations, FrameData, operations, Client, and Server, and let their final callbacks
and typed destructors return before unloading extension code.

During `PrepareEffect`, each selected node receives an `EffectCompileContext&` borrowed for that callback.
Use it directly to declare resources, add passes, read upstream ports and publish output versions. For example,
a build callback can clear and publish a color image as follows.

```cpp
auto image = context.declareResource({"color", GraphImageDesc{64, 64}});
if (!image) return cxx::unexpected(image.error());
auto clear = context.addTransferPass("clear", TransferWork{{ClearImage{{{*image, 0}}}}});
if (!clear) return cxx::unexpected(clear.error());
return context.publish(color, {{color, clear->written.front()}});
```

Recompile extensions against `<lux/engine/render/effect/EffectCompileContext.hpp>`. The compiler owns the
graph draft, finalization and dependency analysis. A rejected choice rolls back both arms, including node output
publications and Resource imports made inside them. Jobs bind a ready device graph and own their per-frame state
and History.

Use `EffectCompileContext::importTargetImage` to declare a sampled input by name, format and sample count. Each
name has its own logical slot. During cold installation a Job supplies a same-device sampled offscreen Target
for each name. Duplicate Target identities select one publication. The compiled graph and its two `sameBinding`
paths can serve Jobs with either equal or distinct bindings.

Use `EffectCompileContext::importResource` to declare a shared input by name, type, projection, shape, required
initialized ranges and boundary state. Compilation assigns each name its own slot. Both the Storage and the graph author can use the same public `ResourceProjection`. A concurrent image declares
its read boundary in that projection. The graph records its accesses even when a
particular Job will disable a branch. At Start or Rebind, `JobBinding::resources` supplies the actual owners.
Their `describeGraphImport` results must satisfy the declared image shape or Buffer capacity, cover the required
ranges and permit the graph's accesses. `resolveGraphImport` supplies each FIF's physical slice. Its size can
grow with the owner's record or page table. Explicit writes remain bounded by the graph's output claim.

The installed [ParticleStorage example](../../../cmake/installed-consumers/render-extension/ParticleStorage.hpp)
declares two 32-byte GPU records. The shader indexes them directly and reads a color at byte offset 16.
The storage owns a buffer and pending rows per FIF. Its command adapter and FrameData column invoke the same
typed batch update. Use the address returned by resource discovery to build a packet before creating any Job.
Staging takes ownership of the selected bytes before recording. A rejected upload leaves
those rows pending; a later edit to a staged row remains pending after settlement.

For a read-only structured buffer, declare the import boundary as `SHADER_BUFFER_READ` and the pass's
logical read as `STORAGE_READ`. This lets the compiler restore the SRV state without requiring a writable UAV
on the native buffer. The independent consumer runs as `compute_output <vulkan|d3d12> particles`. It updates
one row, reads both rows through the shader and checks the pixels from two frame slots after releasing the resource.

CPU snapshot and Raster owner names follow the same static slot assignment. Two names bound to one snapshot
Resource share one capture per Job preparation. Rebinding one name to another Resource selects two independent
values from the same compiled graph. Optional Raster texture owners can remain unbound on a Job.
The Resource publishes a replacement through `ResourceRegistry::publishGraphInput` on the device owner.
Pass a FIF index to update one slice or `UINT32_MAX` to update all slices. The return value counts the updated
bindings. A shared input may be withdrawn. A replacement for an installed output must cover its claimed bytes.
Use `publishGraphBuffers` when one Resource changes several Buffer projections together. Supply the projection,
FIF slot and slice for each change. Writer claims are checked against the proposed final assignments, so an owner
can swap two backings in one call. On failure, all installed bindings and issued product versions retain their
previous values.

For shared images that need different native layouts, give each use its own physical image and projection. For
example, describe the sampled projection with `initial_use` and `read_use` set to `SAMPLED`, and the copy projection
with both set to `TRANSFER_SOURCE`. An owner can opt a projection into cross-queue reads through
`ResourceProjection::concurrent_image_read`. Its native texture must use concurrent sharing. Supply stable,
device-bound preparation and accepted-use frontiers in `ResourceInput::queue_uses` for each physical image.
Versioned buffer inputs use the same fields. Submission waits on the selected backing's preparation and records
its accepted use for the compiled access and release batches. An owner keeps the frontier objects alive through
recording and native acceptance. A publication that changes a frozen buffer slice or its addressed pages returns
`kDeviceBusy` before updating any reader. A publication that changes only the frontier objects takes effect on
the frozen slot after submission settles. The other selected FIF slots receive the update immediately.

Prepare each new image with `ResourceUploader::writeImage`. Its optional `ImageUploadBoundary` selects the sampled
or copy-source layout and collects the actual Copy/Acquire receipts, including an accepted prefix on failure.
Publish the complete set through `ResourceRegistry::publishGraphImages` on the device owner. Each entry selects one
projection and one FIF or the whole family. A failed candidate preserves all installed bindings. An affected slot
with an unconfirmed native submission or active recording returns `kDeviceBusy` and can be retried. After successful
publication, defer destruction of each replaced texture through its own combined preparation and read frontiers.
The compiler only considers asynchronous compute for an imported image with a qualified read-only contract.

Use `importFinalBuffer` for a writable Buffer projection. Its owner grants this capability in
`ResourceProjection::final_buffer_output`. The graph may narrow its claim with `GraphResource::output_range`.
A default claim covers the declared Buffer. Each write is checked against the claim, and exporting the written
version observes that range. For example, two Jobs may write [0, 8) and [8, 16) of the same projection.
A competing writer of [4, 12) receives `kJobBusy`. A copy extending beyond its own declared range fails graph
validation. GPU binding names keep distinct graph slots when several names refer to one owner. Use `sameBinding`
to select prepared work according to their logical identities.

To consume a particular publication, take a successful writer response and set
`JobResourceInput::publication = submitted.bufferVersion({8, 8})` on the reader's binding for that Resource and
projection. The offset is relative to the writer's projection. The reader imports an initialized eight-byte Buffer
whose offset zero addresses the first selected byte. Start or Rebind validates the writer, frame, physical slot and
exported range. An out-of-range selection returns `kGraphInvalidRange`. A superseded receipt returns
`kRuntimeIdentity`. The Resource can serve an intact version after the writer stops.

The selected path publishes a new version once the Job has been accepted and its writes cover the full exported range.
The compiler proves coverage from unconditional writes where possible. For ranges that depend on conditional writes,
publication checks the frozen branch selection. A known rejection before the first write preserves the old version.
A partially accepted write invalidates overlapping versions. A partial Job must be replaced before rendering again. An uncertain submit
uses the device's UNKNOWN retirement protocol. Prepared readers and the accepted physical queue frontiers protect
the backing even after the reading Job stops. A Buffer reader pins the exact publication once the writer
has settled its submissions. Its first access on each physical queue uses the producer frontier as a GPU
dependency, so admission can proceed while accepted writes are still in flight. Start and Rebind reject
cycles between current-run Target reads.

The Resource registry indexes installed readers by Job ID, execution generation and compiled resource slot.
The Job owner resolves that identity for each affected reader. Installation qualifies the candidate directly and
publishes its registrations after layout, writer and dependency checks. Discarding a candidate leaves the
installed registrations available. The registry checks writer claims against overlapping FIF slices in the same
Resource before publishing replacements. Two output names can address overlapping bytes in the same Job when both use the same
projection, retain its canonical state on the same physical queue, and the cold binding predicate disables access
through one name. This also accounts for barriers recorded by a disabled pass. Conflicting names return
`kGraphConflictingOutput`. An installed Job retains its Resource owners. Binding a shared input adds no declared
Job scheduling edge.

The built-in `Lighting` node consumes a GBuffer surface and an optional connected shadow bundle. Its
`direct` implementation binds the four `EDirectLightingBuffer` projections of a scene `LightResource`
and takes its View data from its named `ViewParameterResource::snapshot_binding` CPU snapshot. `SetLightingEnvironment` updates the node's
ambient radiance. Its HDR output connects to `ToneMapping`.
Select `lux.render.lighting.deferred.clustered` to build the bounds, count, scan and fill passes inside the node. The
default grid is 16 by 9 by 24, with logarithmic depth and capacity for 65,536 light indices. Override it
with named arguments such as `x=7 y=5 z=9 index-capacity=4096 depth=linear`. Depth accepts `linear` or
`logarithmic`. Both the list builder and the
deferred shader read the selected row of the Scene View buffer. If a list overflows its capacity, shading evaluates the
complete local light list.

Connect `DeferredLightingPorts::cluster_lists.semantic` on the clustered implementation to consume the GPU buffers that the
lighting pass uses. The bundle carries grid metadata, cell bounds, counts, exclusive offsets, indices,
and status. `ClusterLists.hpp` defines the record layouts. Check status before consuming the indices.
Valid indices use bit 31 to select spot lights and the low 31 bits for the dense light index.

Use `context.onFrame<&onFrame, &link, &createWorkspace>(configuration, uses)` for synchronous CPU work.
The callback takes its linked configuration, typed Resource inputs, and the writer for its declared outputs.
It may take one mutable workspace as a fourth parameter. Supply a link function when resolving raster operands,
and a factory when the workspace needs installed sources. Stateless callbacks use neither an allocation
nor a factory. The Effect owns the linked algorithms for each device. Job installation creates their workspaces.
GPU pass and buffer uses determine liveness. Live callbacks run in declaration order before Freeze.

A frame algorithm reads the selected Resource views in declaration order. Declare each view with
`readResourceView<T>(requirement, operand)` and list its slot in `resource_views`. In `EffectFrameInputs<T...>`,
each tuple element borrows its typed
Resource value and carries its selection revision. All admitted Jobs and aliases bound to the same Resource observe the same view and revision for that round. The device owner releases those borrows before staging can suspend.
Complete shader blocks returned by the algorithm map directly to `inline_blocks` in declaration order.
The writable FIF adopts these blocks after all algorithms succeed. A `RevisionedValue<Block>` may supply a
nonzero revision for unchanged bytes. Pass and buffer uses determine which algorithms are live.

The Resource requirement selects the owning scope and content lifetime. A private view requirement uses the same
manifest as a Capability requirement. A matching binding name must describe the same Resource contract.

Mutable raster programs belong to the Resource that publishes them. Its asynchronous command owns the shader
artifacts and complete pipeline requirements until native preparation returns. The command can run before any Job
exists. A failed replacement preserves the previous publication. A running Job selects a ready table during
CPU preparation and retains the programs through the accepted queue frontiers.
The Mesh instance Storage publishes active groups, mesh sections and material uses once per selection revision.
The built-in indexed mesh passes select already prepared pipelines from those tables on the device owner.
Each Job holds its selected native uses until replacement or retirement. A changed instance selection or a new
material publication for that purpose updates the mapping before Freeze.
Built-in mesh draws retain their qualified vertex and index slices for the ordered mesh/program selection.
Changing a shadow's view count rebuilds its draw layout and keeps those geometry bindings.
Mesh release triggers a generation check before the next use. Geometry handles supplied through an external
frame writer remain valid only for that invocation.

The built-in camera visibility, GBuffer, forward mesh and Shadow capabilities declare a Scene-scoped
`mesh.instances` requirement. Create or query the Scene's `InstanceResource` with that binding name to update
its instances. At Start, all its device-specific GPU projections and its CPU selection resolve to the same
instance. Mesh-to-Transform references and the graph projection both use the Scene’s default Transform storage. Raster passes take their material and geometry draw inputs at installation.

Instance creation validates the entire batch on the Server owner before publishing IDs and retaining Transform
references. Create, release and reassignment mark affected FIF records dirty. The next admitted frame performs
the native allocation and upload from the owner’s current CPU records. A failed frame upload leaves those
records available for retry. These commands update group membership when visibility changes. The Resource packs one draw record per
group with arena offsets, bounds and a visible-member offset for one camera. Candidate slots, chunk ranges,
camera draw records and camera indirect dispatch arguments have one backing per Scene instance/device/FIF.
Raster and shadow preparation borrow the Scene's `MeshDrawTopology`. The instance Resource supplies candidate
indices and chunk ranges to shaders through their buffer projections.
The owner patches packed candidate slots where group membership or offsets change. Each FIF accumulates
the byte interval covering its outstanding changes, including when another FIF renders several updates first.
A new allocation receives the full projection before publication. A rejected copy returns its selected bytes
to that FIF's pending interval. Transform-only updates keep the grouping stable.
Replacing a candidate in the same group can change the membership projection while preserving the
visibility records and raster commands. Shadow selects group records and dispatch dimensions by their
published content. With one layer it reads the Scene's group rows directly during CPU preparation.

The camera visibility implementation publishes group records, visible members and indexed indirect commands.
Visible members and indexed commands are writable and private to the Job. For camera-only culling their sizes come from the admitted instance resource slot and the shaders read the Scene View GPU row. Raster output also selects a viewport.
Shadow projection uses the light layer count for its layout. A camera position or direction change updates
the light-mapping header. A changed inverse view-projection matrix updates directional cascade views.
Point and spot views keep their light-space matrices across camera movement. Slice data has its own upload revision.
The instance Resource counts enabled uses of each
material within a mesh/program pair. Adding another instance to an existing group can update GPU candidates
without changing the program slots or the material read list. Raster programs follow first use in GPU group order.
Jobs share the program slots and distinct material reads when the instance source, geometry source, material
owner, purpose, winding and raster contract match. Before the CPU draw algorithms run, input selection resolves changed mappings against
the mesh Resource's vertex layouts and binds the ready programs. The draw algorithms consume those slots. A material publication invalidates selections using the affected program and purpose.
Each Job retains its own native uses through its accepted queue frontiers. The material owner indexes active
selections weakly, so removing the last installed program binding releases the shared CPU data.
The address-enabled implementation reads both instance fields through the `PAGED_ROOT` projection.
The node defaults to frustum culling. Pass `frustum-culling=false` as the node argument to keep every enabled instance
with a nonsingular transform. Each group has one command per winding. Consumers use the bundle inside
the same graph, where the compiler schedules the visibility writes before their reads.

Use `capabilities::DeferredGeometry` to connect the built-in indexed mesh surface to `DeferredLightingPorts::surface.semantic`.
The node runs camera visibility and consumes the same grouping in its RasterStream.
Choose `lux.render.gbuffer.indexed` or `lux.render.gbuffer.indexed.clockwise` for the authored front face and use
the same view and scene bindings as MeshVisibility. Bind `mesh.geometry` and `mesh.materials`
to the Job's Mesh and Material owners. Bind `mesh.textures` for programs that sample the shared texture table.
The surface bundle includes albedo/metallic, normal/roughness, emissive/ambient occlusion, and forward-Z depth.
Connect the optional `DeferredGeometryPorts::draws.semantic` output to consume the same visibility bundle as the raster pass.
When only this output is demanded, the implementation builds visibility without the raster pass or its material bindings.

Use `capabilities::ForwardGeometry` for a single camera raster pass with the same Mesh and View bindings.
The forward implementations use cooked forward materials and publish an RGBA8 display color. The
depth-only implementations use the cooked shadow material path, including AlphaMask, and publish
forward-Z camera depth that can feed `DepthPyramid`.

Use `capabilities::ShadowRendering` to connect the indexed shadow caster path to `DeferredLightingPorts::shadows.semantic`.
Choose `lux.render.shadow.indexed` or `lux.render.shadow.indexed.clockwise` and supply the same Mesh and Material bindings as GBuffer. Installation resolves the current View and the Scene’s Transform and Light resources. The shadow projection reads the selected lights through `shadow.lights`. Optional
node arguments default to `resolution=1024 distance=100 pcf-radius=1`.
Pass `frustum-culling=false` to disable frustum culling for shadow casters.
The number of active cascades and light faces determines the depth array layers for each prepared frame.
The RasterStream applies each light's depth bias and publishes the layer and light mappings with the depth image.
The optional `ShadowRenderingPorts::draws.semantic` port exposes the visibility groups, members, and indirect commands used by the caster
pass. The command table contains two winding streams per active view of each group.

Use `declarePreparedBuffer()` for read-only graph buffers filled by CPU algorithms. List uploads in
`FrameAlgorithmUses::uploads` and graph-written extents in `frame_sizes`. Call `EffectFrameWriter::publishBuffers()`
once with the complete payloads and byte counts in their declaration order. A nonzero upload revision identifies
unchanged bytes for this algorithm and installed Job. Each FIF replica tracks its native allocation generation,
accepted revision and size. A matching revision and size reuse the resident bytes. For a changed upload, the
device round owns a copy before the algorithm returns. Synchronous input selection binds the safe FIF allocation.
Only changed uploads enter the device's copy list. Each entry borrows its selected replica and the owning
allocation's queue frontiers for this round. Staging can span multiple windows. For each accepted window, the
receipt advances the replica's contiguous byte prefix and registers the queue use on that allocation owner.
A version is resident once the full interval has been accepted. An unknown outcome invalidates that replica's contents.

For graph-written buffers, `declareFrameBuffer(name, minimum, maximum, ResourceBufferExtent{binding, field})`
takes its byte count from a declared resource. The storage's `bindBufferExtent` returns a stable read-only row
with one entry per FIF slot. Start binds the row and retains its owner. The owner sets the admitted slot's byte
count during resource preparation. Before Freeze the Job reads that slot and reuses or grows its own buffer.
The camera visibility path uses two such fields for visible members and indirect draws. Camera-only culling
has no CPU snapshot or preparation callback. Shadow's CPU projection publishes its view-dependent extents alongside its upload data. Both paths require whole-buffer graph accesses, a GPU write before any read,
and four-byte-aligned sizes within the declared bounds.

For a pass that consumes a complete constant member, declare the pass's typed inline range, then call
`context.copyInlineBlock<Block, State, &State::member>(pass)`. The compiler checks the block's type, size,
alignment and target layout and rejects a second writer. When the Effect publishes its device graph, it binds
each live pass to its state member once. Jobs borrow these bindings. After synchronous preparation succeeds,
the device round compares the member directly with the pass's inline bytes for the selected FIF slot.
ToneMapping uses this declaration for exposure and Deferred Lighting for its environment parameters.

Use `capabilities::ToneMapping` to author the built-in HDR-to-display node. Choose the `linear` or `srgb`
implementation, connect a `lux.color.linear-hdr` input to `hdr_color`, and consume the display value from
`color`. Pass an optional `exposure=1` in stops. Exposure defaults to 1.
`SetToneMappingExposure` addresses an Effect and a node, including before a Job is attached.

Built-in node arguments use named options from the installed `lux-cxx` arguments component
(`lux/cxx/arguments/TypedOptions.hpp`). For each live node, `CreateEffect` calls its registered
`ImplementationEntry::create(NodeId, arguments)` factory once. The returned `EffectImplementation` owns its parsed
settings and any command state. Its constructor can bind a state member with `bindState` and charge retained
configuration bytes to the Effect budget. Each device build calls the same object's const `onBuild` with its own
compile context. Builds read immutable settings while owner-thread commands act on the bound state.

The built-in GBuffer, Shadow and Lighting headers publish their respective bundle schemas for Effect producers
and consumers. Use the schema's type, version and member names when declaring or connecting these bundles.
Cold graph failures report an `EffectDiagnostic` through `RenderErrorSink` with the source node and pins where
available. The sink may retain a copy of this value after returning from the callback.

For a producer-authored Raster stream, declare the program choices and read-only resource envelope on
`RasterPassTemplate`. List the owned pass IDs in `FrameAlgorithmUses::draw_streams`. During cold linking the compiler supplies a
`std::span<const RasterOperands, N>` to `resolve(config, streams)` in declaration order. A fixed extent checks
the number of streams before entering the callback. Retain the slots returned by `stream()`, `programAt()`,
`buffer()`, `image()`, `sampler()` and `argument()` in linked data. Buffer and image slots identify their stream
and the first row of a compiler-grouped access range. The frame checks the draw's use and slice within that group.
Each live program uses its own reflected
layout and native program lease. Program preparation maps each argument and array element directly to its
descriptor use. Each writable FIF counts all private draw sets before growing its pool, then resolves each supplied
binding once and writes sets by program and draw ordinal. `setRasterStream` copies the supplied arrays into the
Job's workspace for that algorithm output. A Resource-authored list supplies its own selected FIF data.
Freeze borrows both kinds of list synchronously. The device round reuses its temporary input table across Jobs
and releases the views on return, including on failure. Supply one binding for each declared descriptor array element.
The writable frame validates
program identity, constant size, geometry, buffer bounds, image subresources and sampler comparison mode before
materializing native draw packets.

For a known set of raster pipelines, declare each use in the pass template's ordered program table.
Each entry names a graph shader, graphics state, parameter bindings and geometry layout. Several entries may
share a shader. During CPU linking, `RasterOperands::programAt(stream_index)` resolves the
entry to a pass-local slot. Keep that slot in the linked draw data. `PrepareEffect` creates the
native pipelines before any Job starts. Jobs can change draw order, geometry and scissors using the linked slots.

Draw and section data are declared in `lux/engine/render/graph/DrawList.hpp`. For mutable programs, declare
`RasterPassTemplate::ReadyPrograms{}` and a typed draw-resource input. Store a `RasterProgramSet` in that
Resource, construct it with `ResourceCreateContext` and await `replace(description)` in a Resource command.
The description specifies attachment formats and sample count, the logical stream inputs, shader artifacts and
ordered graphics layouts. Multiple layouts can reference one shader. Use the same input declarations for the
pass and its Resource. A successful command publishes the entire set. Jobs share immutable linked rows with the publication and keep
their own draw-slot ordering. Replacing the publication leaves the rows used by other Jobs alive. Each Job releases
its native pipeline uses with its accepted queue frontiers. The device idle-cache budget applies after the last use retires.

A Resource exposes its stable program set through the factory binding operation `rasterPrograms`.
Declare the Resource requirement in the capability and name it in `RasterPassTemplate::ReadyPrograms`.
Start and Rebind resolve the set from that scope. Frame preparation selects its latest successful publication
before the draw algorithm runs. Use `frame.rasterProgram(stream, index)` for the pass-local draw slot.
Its revision changes when the linked program sequence changes.
A PER_RUN Resource can also supply the draw list directly for a pass with a named program source. Its
pass-local program ordinals have revision zero and address the set selected for that Run. Changing the
publication updates program bindings before Freeze, even when the pass has no CPU algorithm.

Declare the Resource owners a draw stream reads in `RasterPassTemplate::draw_resources`. Each entry has a
binding name, logical Resource type, declared reads and optionality. Start and Rebind
qualify the supplied Resource against the draw contract already declared by the Implementation.
Job bindings supply the name, actual Resource, projection and optional published version. Installation resolves
scope defaults directly into the compiled operand slots. The same slots feed GPU imports, CPU views, draw owners
and same-binding predicates. CPU algorithms declare the Resource data they need with
`readResourceView<T>()` and receive that read-only projection in `EffectFrameInputs<T>`. A draw access is
linked by binding name and logical Resource type with `streams[i].resource(name, Descriptor::descriptor.type)`.
The resulting `RasterResourceSlot` indexes one compiled source and its draw output. During workspace creation,
bind the Mesh, Material or Texture source using the supplied `InstalledDrawSources`. Keep that binding in the
workspace and use it only during the workspace's frame callback. Start and Rebind validate the resource type,
read access and output at installation. The callback selects live item IDs and the current program revision.

For shared sampled 2D textures, declare the logical texture Resource type and require `SAMPLED_IMAGE_ARRAY`. Runtime programs
can use the device's shared sampled-image and sampler arrays. Install a `TextureDrawBinding` and call
`sample(texture_id)` for each texture the program can access. The result supplies its image and sampler indices.
The renderer validates the texture identity against the bound owner and tracks the selected images through
native submission.

For reflected material pages, declare the logical material Resource type and require `STORAGE_BUFFER_ARRAY`.
A stream that reads only the material program declares the owner with zero required capabilities.
Declare the unbounded read-only structured buffer argument with
`RasterPassTemplate::Input::shared_records = true` and give it the name reflected by the shader. The built-in
material adapter uses `uMats`. Declare the shared sampled texture and sampler arguments the programs use. The renderer prepares the writable
frame's material replicas before the producer runs. Call `MaterialDrawBinding::bind(sources, records, textures)`
at installation, then `record(program, material_id)` during preparation to obtain the GPU page and record indices.
Only the selected draw
programs contribute their exact page bytes and sampled subresources to the frozen reads.

An external Resource with its own structured records can use `frame.rasterRecord(source, program)` directly.
Its writer borrows the reflected block and writable frame slot for this preparation. Publish a single native
record slice with `write(input)`, or use `write(input, image_source)` when that record also reads sampled
subresources. The writer checks the record stride, bounds and all image operands before appending any reads.

For uploaded indexed geometry, declare the logical mesh Resource type on the stream.
Install a `MeshDrawBinding` and call `geometry(mesh_id)` during preparation. Use the
returned vertex slice, index slice and index type. Mesh slices are scoped to the current stream and
preparation. When the source geometry and shader use different attribute locations, supply
`geometry_locations` in `graphics.vertex_attributes` order on the stream program. An empty list selects
the identity mapping. The graphics state keeps the shader locations for native pipeline creation. The renderer checks the program's vertex layout, index width and slice bounds and tracks
the selected byte ranges through native submission. Indexed draws using selected Mesh vertices bind the
whole vertex slice and use the corresponding mesh's indices with zero base vertex. The producer keeps
GPU-written indirect arguments within those bounds.

The public layout element is `GeometryAttribute` and the selected pair is `RasterIndexedGeometry`. Existing
authors using `MeshAttribute` or `RasterMeshGeometry` should update those type names when recompiling.

An SDK Resource can supply indexed geometry directly. Set `reads = EDrawResourceRead::INDEXED_GEOMETRY` on its named Pass input, link the typed
slot, and pass an `IndexedGeometryInput` to `bindIndexedGeometry()`. The Resource owns both native buffers and
supplies a stable item identity, byte ranges, strides and vertex attributes. The Job copies the attributes during
the call. The returned slices are valid for that stream and preparation. Indexed draws require a matching vertex
and index selection from the same owner and item; repeated selections of the same backing pair may be combined.

For indirect commands addressed from the backing buffers, set `RasterStreamIndexBinding::origin` to
`ERasterIndexOrigin::BUFFER_BASE`. Use the selected mesh's `arena_first_index` and `arena_vertex_offset`
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
must agree on that count before any image is replaced. The compiler assigns each runtime-layer image a slot.
At that slot each device FIF owns the texture and its single-layer views. Freeze uses the section's layer index
to select the attachment view. Direct vertex and indexed draws can use vertex
buffers, read-only private buffer bindings, sampled 2D or 2D-array views and complete per-draw inline blocks.
Draws can reference the same binding slice. Successive uses of that slice by the same program share a private
descriptor set for the frozen frame, including when another program draws between them. Mesh surfaces use
one slice for the whole visible-member buffer when indirect first-instance addressing is available.
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

A Capability can declare a scoped GPU input in `EffectCapability::resources` with its logical binding, Resource
type, domain and content lifetime. An implementation passes the full requirement to `importProjection` for a GPU
operand or `readResourceView<T>` for a CPU operand. Both can also declare implementation-private requirements.
During PrepareEffect the compiler links each consumed scoped requirement to its registered factory and instance key.
Its manifest maps each resource-backed raster stream to its program and DrawList sources and each dynamic extent to its field.
Compilation checks device capabilities, content lifetime and required factory operations, and binds inline and UBO
fields to the configured Effect state. The device attaches UBO storage before the Effect publishes the linked graph.
Start and running Rebind resolve the instances in manifest order and check their actual projections and frame spans.
The manifest also records the registered type and device compatibility of each explicit operand. Binding checks
the supplied instance's generation, type, device and current projection. An optional operand can be omitted even
when its factory is unavailable on this device. Failure releases instances created by
that candidate. A previously published instance remains accessible through `QueryResource` with the same device,
scope, type and binding. An empty binding selects the default instance. The installed Particle example binds two
VIEW-scoped instances of the same type and updates individual rows by ResourceInstanceId.

The registry owns the type description, configuration and callback table in a stable row for each registered type.
A new instance keeps a reference to that row through construction, updates and teardown. Typed frame updates take
one span per declared column. A member update can take four spans. A free update takes the owner and up to three spans.

A JOB-scoped resource belongs to one installed execution. Query its named instance using the running Job ID.
Rebind creates a new private scope. Closing the old scope rejects new commands immediately and honors commands
that already borrowed it. The resource owner retires its native objects at their accepted queue frontiers.

A named requirement selects an independent instance keyed by its logical binding. A requirement marked
`scope_default` selects the scope's unnamed instance. Built-in visibility, surface and lighting implementations
use this for Scene transforms, the current View row and Scene lights. Their CPU operands and shader projections
resolve to the same instance. An explicitly supplied operand must agree with that installation.

Scope and content lifetime are independent. A PER_RUN owner receives typed updates at its resource ID.
At admission, `selectRun` consumes pending content once for the selected resource and safe FIF slot.
A Persistent resource can use the same call to fix the value its CPU and upload readers share. Scene View selects
one row revision per device round. All Jobs reading that View use the same parameters and upload revision, even if
a different connection updates the logical row while the device waits for staging space.
With no payload, the owner selects empty content. An owner requiring input supplies `hasRequiredInput`.
The installed Debug Geometry consumer packs vertex and index data into one host-upload buffer per Job and FIF slot.
The draw's first index accounts for the vertex bytes at the start. Above the current capacity, the owner writes
a candidate backing, publishes its projection to the installed readers in one operation, then retires the old
buffer at its accepted frontier. A rejected publication discards the candidate. Empty inputs keep the capacity.
Its Capability
declares the geometry resource. Its RasterPassTemplate names that binding in `draw_list` and declares the vertex
and index reads in `resources`. At Freeze the installed `readDrawList` operation lends the selected FIF arrays
for the duration of the synchronous call. Each draw selects a fixed program by ordinal. Vertex and index slices
index the declared buffer accesses in order, and image slices index the declared sampled image accesses in order.
Resource-local slots leave the stream ID at its default. The optional `clip` has half-open framebuffer pixel
edges and intersects the pass or section viewport. An empty list still performs the authored attachment operations.

A finite sampled image argument on a Resource DrawList can declare a `target_format`. A
`RasterTargetImage{target, read}` binding then names one sampled single-sample offscreen color image.
The owner previews the pending list when `readDrawList` receives `std::nullopt` and supplies that
selection again at the admitted FIF slot. Only binding ranges referenced by draws participate. The
device resolves generations, format and eligible writers before admission. A current-run read orders the
producer's output before the consuming batch. A latest-published read selects the existing publication
and waits on its actual writes. Both modes may reference the same Target in one list. Bindings in the same
consumer batch share one read per Target and read mode. The first raster batch reading that publication
waits at the combined shader stages of all its raster reads. Graphics queue order carries the dependency
through later batches. Each accepted batch extends the selected image's retirement frontier. Freeze checks each binding against
the Target, mode and argument selected during admission. A missing current writer, stale Target or cycle fails
the request. Input updates already adopted by its window keep their effects.

The installed Scene View shader declaration is `lux/engine/render/resource/view/SceneView.slang`.
The overlay sample imports the View row alongside JOB-scoped geometry. Its two passes consume the same selected
DrawList in disjoint viewports. One program reads the View color and another uses its own fragment shader.
Both Jobs on the first View use that View's GPU row while a third Job on the same Effect reads the other View's row.

A Resource factory can call `ResourceCreateContext::require(type)` to acquire a dependency in its owning scope.
The Registry records that relationship against the instance under construction and keeps it through rollback and destruction.

Resource upload is independent of Effect creation. `GetOrCreateResource` selects the owner in the
device, Scene, View or installed Job domain. Resource commands route through the full owner or item identity.
An EffectGraph names the resources it uses. At Job assembly the Server qualifies each supplied owner,
its binding projection and the resource contract before creating an execution.

`EffectCatalog` owns capability definitions and registered implementation factories. A capability defines semantic ports
and schema versions. Select an implementation and connect the ports in `EffectGraphEdit`. For a deferred
scene, connect the `GBuffer` surface to `Lighting` and its HDR result to `ToneMapping`. Connect the
`Shadow` output when needed. The clustered lighting implementation also exposes its list buffers as a
separate output.

Register Resources, capability definitions, implementations and command bindings before `RenderServer::run()`
seals the catalog. Resource registration owns the corresponding command and FrameData types. Node commands
route to the state member bound by the configured implementation. The host keeps plugin code loaded through
Server drain and destruction of the last implementation.

## Requests and execution

Framework commands are split by Scene, View, Target, Effect, Job, Discovery and Server responsibilities.
`runtime/commands/Commands.hpp` is their aggregate. A Job owns its logical Scene, View and Effect relationship from Create
to Remove. Start selects the Target and builds a private device execution. Stop closes future frame admission and
returns writer eligibility while an admitted Run retains its execution through native acceptance. Remove requires
those borrows to settle. Rebind checks a candidate before publishing a new Effect or Target on the same JobId.
Mesh and Instance commands have independent namespaces in `runtime/commands/MeshCommands.hpp` and `runtime/commands/InstanceCommands.hpp`.

The host serializes catalog setup before calling `run()`. The call seals registration and transfers the
catalog to the Server thread through shutdown. Runtime clients discover IDs through the fixed
`QueryCommandIds` bootstrap; `RenderClient::create` uses that IPC path. Accepted commands use IDs and retain
the binding qualified for the actual Resource.

A Resource command declaring `requires_idle_resource` is exclusive for that instance. A conflicting
operation returns Busy, and Jobs that need a Resource with an exclusive command in flight wait for its
terminal delivery. Unrelated IPC, Jobs and transfers can proceed.

Use `registerCommands` with `CommandBinding::declare<C, function>()` for stateless typed sync/async functions.
For a synchronous node command, register `CommandBinding::declareForEffect<C, State, function>()` with its
implementation entry. At `CreateEffect` the Server links the full command identity and node to the configured
object's bound `State`. It rejects ambiguous routes and a mismatched state type before publishing the Effect.
The typed method receives its state by reference and acknowledges CPU updates.
Stateful Handlers retain constructor-injected dependencies. All forms use the existing request operation,
Resource borrow and ResponseOutbox; an async request's source bytes remain borrowed until its real response.

`JobExecution` owns CPU input bindings and Resource subscriptions. The Effect's device graph owns each producer's immutable linked inputs.
Declare a frame algorithm with `context.onFrame<&Algorithm::onFrame, &Algorithm::resolve>(config, uses)`.
A stateful algorithm can supply `&Algorithm::createWorkspace` as the third template argument. The Job owns
the resulting workspace. At frame selection the algorithm receives its installed Resource views and writes
the declared buffer, draw or inline results. The compiler checks each live inline destination's shader ABI.
The device graph stores each call's output positions, byte ranges and destination passes. Each Job owns one staging
allocation. After all live calls succeed, the frame applies the selected blocks and invalidates the affected recording
units when their bytes change.

The Job owns each workspace, its CPU binding rows and their revisions. Each DeviceEntry owns the Job's native
buffers, images, memory allocations, descriptor pools, views, command buffers and History backing. The device indexes
the stable allocation by JobId and checks the execution generation at cold binding and retirement boundaries.
Cold assembly resolves Resource borrows once. Instance preparation uses those borrows and contributes preparation copies.
After Target prefetch, FrameProgress opens the next strict FIF slice for the ready Jobs. The device round prepares
Resources and freezes recording inputs on the Server owner before dispatching graph work. When staging fills,
the round submits the copy and resumes preparation on the owner using its receipt. A round waiting for History
or GPU capacity resumes when the device makes progress. Job teardown releases its workspaces while
the Effect still owns the prepared graph. The CPU state releases its native borrows before the device queues the physical
collection for retirement at its accepted queue frontiers. The retirement callback requires only NRI and owned objects.

Each FIF freezes the native operands consumed by its passes before recording. Fixed Raster draws use
vertex descriptors and resolved buffer addresses for indices, indirect arguments and optional counts.
The physical bindings borrow immutable NRI descriptions for the selected buffer or texture. Draw sections and
slice validation reuse those descriptions until replacement. Texture and material producers snapshot the
selected texture's mip and layer counts for external reads.
Initial preparation fills all rows. Later changes use the compiler's resource-to-pass index, including
fallback operands and WSI tails. Viewport changes visit the raster passes that inherit the viewport.
History rotation updates the selected views and initialization commands. A failed freeze retains the
pending rows for retry and leaves that FIF unavailable to record.

The compiler indexes reusable recording batches by resource. The index includes command operands, fallback
inputs, attachment views, extents and barriers, including final transitions. Changing a physical binding
marks each dependent reusable batch dirty once until its next successful recording. The WSI tail is always
recorded separately. Vulkan table replacement checks the changed descriptor kinds against fixed layouts and
the selected draw packets. Replacing a frame's private descriptor pool invalidates its recorded pool bindings.

Each FIF command batch owns its allocators, command buffers and recording versions. A batch recording lease rejects
concurrent recording of the same batch; independent batches have separate allocators and per-pass scratch. The caller
must join record workers before preparing, resizing or destroying the instance. The round retains the slice until
every accepted queue frontier completes; a device terminal prevents further preparation or recording. Direct internal
recording callers must enforce the same completion boundary. The recording lease does not replace GPU completion.

`prepareFrame()` copies parameter bytes and freezes resolved native views into instance-owned FIF storage. Client
vector mutations do not alter those bytes. FrameExecutor only traverses the Plan and calls typed encodings with
stable slices; it performs no resource discovery, compilation or submission.

Set `GraphImageDesc::volume_depth` to declare a 3D transfer image, including one whose base depth is one.
Volumes have one array layer and one sample. Width, height and depth shrink independently down to one at each mip.
`GraphImageRange` addresses the mip for synchronization. `ImageCopyRegion::z` and `depth` select the planes to copy.
An image-sized dispatch uses the selected mip's width, height and volume depth. `ImageDispatchArguments::group_depth`
defaults to one and controls the Z workgroup size independently of array layers. Fixed extents compile to direct
dispatch counts. Target-relative extents and extents supplied by imported images resolve against the selected backing.

Image/Buffer copies distinguish capacity from initialized content. Explicit pitches define the copied rows within
each plane. Padding between rows and planes retains its previous contents. Use a destination `READ_WRITE` to
preserve an already defined range when the write footprint depends on the device-selected extent or pitch. Count and indirect argument accesses retain their actual offsets and byte ranges even though NRI
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
   Each device owns its frame slots and upload state. Each sampled offscreen Target owns its published image. The Server visits ready devices
   in rotating order. QueryRenderDevice reports a device's execution error separately from the query status.
   A terminal device error closes its admission while healthy devices continue submitting.
   Each entry in `JobBinding::target_images` names a sampled Target and selects an `EProductRead`.
   `CURRENT_RUN` requires a writer that will produce that Target in the selected round. A requested writer needs
   its own request. `LATEST_PUBLISHED` selects the last complete image before Freeze, including when that same
   Target has a writer in this round. An unavailable product fails the request with `kRuntimeState`. An image
   still in GPU use follows its completion frontier. The two modes may be used together on the same Target.
4. Build reusable typed packets with addresses from `QueryResource` or `GetOrCreateResource`.
   Move the packet into `UpdateFrameData::Request` to apply it. A `RequestRenderJob::Request{job, std::move(packet)}`
   carries the same resource updates and requests one submission. A continuous Job may run without a packet.
5. Reuse a slot on its terminal reply. An update reply confirms input application. A successful exact Job
   reply confirms native submission acceptance. Actual GPU completion governs physical FIF reuse and Target
   retirement. For window Targets, presentation is a separate native outcome.

Both operations preflight server identity, resource generations, registered channels and exclusive borrows before
writing. Each owner then validates and applies its batch in packet order. A later value error preserves earlier
successful updates. Resource updates remain usable across Job Stop or Rebind while their addressed owners live.
An exact request that fails has its own terminal
and can be retried after its cause is resolved. A continuous Job with a failed round exposes `last_run_error`
through `QueryExecutionProgress`. An explicit request, execution input change or cadence change can retry it.
The installed Render SDK sample exercises scoped resource updates, exact Jobs and GPU pixel readback.

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

Fixed Raster, Compute and Transfer compilation resolves resource versions into typed operands. Freeze selects the
current History bank or fallback, checks the bound slice, and resolves indirect offsets, dispatch dimensions,
compressed block regions and copy pitches. Recording consumes those per-FIF native operands. Resize validates
candidate image extents before adoption and the next Freeze resolves the new physical bindings. Dynamic producers link `RasterStream` slots during
compilation and supply draws and resource bindings through `EffectFrameWriter`. The graph declares attachments
and fixed graph resources.

`GraphDraftBuilder` indexes each program’s reflected fields when the program is added. Raster, Compute and Transfer
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
The Effect compiler owns lowering, resource liveness, native program acquisition and linking of CPU inputs
and output bindings. It publishes one immutable `compiled::RenderGraph` per Effect and device after these stages succeed.
The graph contains the native program uses, recording, resources, submissions, CPU preparation and linked imports.
Jobs borrow this graph and keep their own mutable state. Before dispatching CPU recording, the control owner
selects one frozen frame per Job. Workers receive the command inputs, batch revisions and device-owned native batch
slots. Each command buffer begins with independent binding state and confirms its recorded revision after a successful End. Source diagnostics can be discarded separately. Native program bindings
release their device-cache uses on destruction, including when a later link fails.

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

At `CreateEffect` the Server validates the authored graph, initializes live nodes and binds their commands.
`RenderGraphCompiler::compile(effect, config)` lowers that configured Effect on the compile worker. A node reads
the original output bundle through its validated pin connections. Each CPU Resource view refers to its import
operand. Graph liveness discards unused frame algorithms before native pipeline preparation.

The Effect publishes its GPU graph, Resource requirements, binding contracts and linked frame algorithms together
on the device owner. Jobs borrow the installed device variant. Native-only callers can request a
`GraphCompileDiagnostics` report through `GraphCompileOptions::diagnostics`. Production compilation skips
collecting resource names, descriptor-field diagnostics and reuse reasons. Barrier planning detects shared physical
slots from the allocation plan. The compiled graph packs pass-entry texture and buffer transitions into two
tables. Each pass selects a contiguous range from each table. Each FIF slot uses the same ranges for its native
barriers. History rotation, backing replacement and Resize patch the affected rows by absolute index.

Shader reflection supplies
layouts and descriptor writes on SPIR-V and DXIL. Fixed descriptor operands and initial parameter bytes are stored
in contiguous tables indexed by pass ranges. Descriptor pool counts and instance parameter copies are computed
during compilation. For fixed samplers the compiler validates ordinary/comparison use, assigns dense slots to live
declarations and resolves their descriptors through the device cache. Jobs sharing the compiled graph use those
descriptors when populating or growing their private sets. Program preparation returns to the owner
scheduler for publication through the existing `CompiledPrograms` cache. The device execution owns those native program
leases alongside the executable. The executable's recording tables carry slots and the Instance freezes their physical
bindings for the recording workers. Barrier predecessors and History predicates use compiled slots. The diagnostic
sidecar carries source Pass IDs, resource names and reuse reasons.

CreateEffect fixes the logical graph and its output format, sample count, Target kind and sampling capability.
For a window, use `TargetCreated::description.outputContract()` to obtain the actual swapchain format selected by
the backend. Offscreen callers can derive the same contract from their declared TargetDesc.

For Windows D3D12 linear HDR, set the surface's `TargetDesc::display_mode` to `EDisplayMode::HDR`.
A successful creation returns RGBA16_SFLOAT and the display's SDR white in nits. Composite UI in linear
scRGB with reference white scaled by `sdr_white_nits / 80`. The output Effect can use
`lux.render.tone_mapping.hdr_linear`. The pinned Vulkan swapchain interface does not report the selected
color space, so HDR presentation requests on that backend return `kDeviceUnsupported`.

To switch an installed surface writer, prepare the new output Effect on its device, then pass its ID as
`ResizeTargetRequest::output_effect` with the requested display mode. The command retains the Job's inputs,
TargetId and activation, and adopts the new native output and execution generation together. A preflight
rejection keeps the previous output. Once WSI retirement has begun, check the reply's `output_available`
before using or retrying the Target. Send `display_changed = true` after a display change or monitor move
to recreate the chain and query its current display even when the window extent is unchanged.
Call PrepareEffect with the Effect and DeviceSession before starting or rebinding a Job. The Effect owns one
immutable graph per prepared device. Repeating Prepare on a ready device succeeds immediately. A second cold
Prepare on the same Effect returns Busy while compilation is in flight. A failed Prepare can be retried.
Start and running Rebind synchronously validate the actual Target, imports and output ownership before installing
the candidate. Resolving an import holds its Storage backing until the binding transaction settles.
A reentrant publication to that Storage returns Busy. Other owners can still publish to their installed users.
Candidate subscribers and final Buffer version records become visible together after layout, writer and DAG checks.
Each installed subscriber refers directly to the graph's binding table and final resource slot. Backing publication
updates its FIF rows through that table. Rebind publishes the candidate table and unlinks the retired subscribers
before destroying their destination. Resize updates the target description at the address used for image qualification.
A failed Rebind leaves the installed execution usable. Stopping every Job retains the prepared graph.
ListEffectJobs includes stopped Jobs until removal or a successful Rebind changes their logical Effect.
Removing a device releases its graphs. Uniform buffers remain in the device's Effect pool until Effect or device removal.
The compiler links producers and constant writers to final slots. Job assembly borrows the plan and creates its own
producer instances, writable frame bindings, transient resources and History.
Standalone project GPU consumers can bind physical inputs directly.

The device graph contains a recording program with typed Raster, Compute and Transfer packet tables,
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
other component tests. On Windows, `LUX_RENDER_GRAPH_ALLOCATION_PROBE=ON` builds the on-demand measurements. Run
`render_graph_chain measure vulkan` or `render_graph_chain measure d3d12` for allocation counts and timings.
These count Render DLL calls to C++ `operator new` on the calling thread during Prepare and Record. GPU timings
bracket the graph command buffer on the same graphics queue. CPU submit timing includes the two timestamp commands.

When one unconditional graphics command defines an entire transient buffer, subsequent readers of that version
can run independently on different queues. The compiler publishes their common read access after the writer
and places the foreign queue wait at the first command interval that needs it. A command sequence or condition
region stays in one interval. Physical reuse still requires every reader to precede the next occupant.

The same option builds `render_alias_overlap <vulkan|d3d12> <single|multi>`. This probe records GPU timestamp
pairs around two independent compute dispatches in their production command buffers. It prints accepted queue submits,
native waits and explicit graph allocation bytes across resize. Each extent has eight warmup frames and 32 measured
frames with one frame in flight. Both compute outputs and the pixels from the aliased images undergo GPU readback.
The multi-queue run exercises an eligible asynchronous schedule. Exit code 77 indicates unavailable topology or timers.
Add `inflight-resize` to measure simultaneous live, candidate and retiring allocations. After the first resize,
the old completed extent can retire while an unrelated queue wait is pending. The probe then submits a separate
256-pixel Job behind the fence and retires it while preparing a resize from 384 to 512. It records the actual
accepted and completed serials, checks that all three sets of allocations stay live until the fence is signaled,
and validates the intermediate and final readbacks. Timestamp capture is disabled for this memory-only run.

On Windows, `render_output_test --start-fairness <vulkan|d3d12> [reverse]` pairs a Start request on adapter 0
with an exact frame request on adapter 1. The optional argument swaps the adapters. Each round creates a fresh
Effect, explicitly prepares its device graph, starts the first Job, then binds a second Job to an equivalent target.
The probe pairs each operation with a peer frame and checks that Prepare compiled once and both Starts reused it.
Each `PAIR` row reports the round, operation (prepare, first binding or repeat binding), previous peer submission
acceptance, operation send, peer send, peer native acceptance, peer reply receipt and operation reply receipt.
Times use the same monotonic clock in nanoseconds. The first eight of 108 rounds warm up all three conditions.

On D3D12 devices using legacy barriers, the compiler returns an unsampled offscreen target with no requested
final use to COMMON at the end of the graph. This matches the initial state of the next submission for that target
slot, including reuse of an already recorded command buffer.

`FrameExecutor::recordBatch` owns the exclusive per-batch/FIF allocator reset and command-buffer Begin/End.
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
instance resource diagnostics report actual Memory groups, requested bytes, placement rejection reasons and
instantiated barrier scopes. Creation and Resize select the same handoff template using their actual allocation groups.
The compiler retains per-physical-queue uses by byte or image range, read/write mode and native scope. A prior
same-range transition can cover an incoming use when both of its native scopes match. Source scopes retain reads
and writes. A handoff from another physical queue can narrow its scope when the entry batch directly waits for
all previous uses at the required stages. For an image, the compiler also checks the recorded layout transitions
of each incoming aspect, mip and layer against the global barrier. Missing coverage retains all-stage ordering.
D3D12 texture placement preserves matching access masks, and legacy texture barriers keep all-stage handoffs.

The RenderGraph owns contiguous tables for batch dependencies, successors, imported resources and History uses.
The compiler includes a draw-source list for each batch when development metrics are enabled.
Every Job reads its local successors directly from the compiled table. Admission orders the selected Jobs by
their current-run product reads. Each FIF slot groups consuming batches by producer Job and merges their
destination stages for repeated reads by the same batch. A consuming batch becomes eligible after its local
dependencies and all required producer Jobs have been accepted. Each completed producer supplies the actual
tail serial from each physical queue for the consumer's GPU waits. Each batch specifies the exact pass range
for each native command buffer. Allocation, fixed binding selection and recording use these same ranges.
A swapchain output tail has its own command buffer,
including when only the final output transition remains to record.

SubmissionOperation advances an ordered, frozen submission window across Queue capacity limits. Each accepted batch records its
actual queue serial; dependencies wait for that producer, never an unrelated later submission. Compatible ready
intervals from multiple Jobs share a submit. The first batch using an import on each physical queue waits for its selected preparation serials. Its wait mask
covers all uses on that queue, including later batches. Any explicit transition or unknown stage retains an
all-stage wait. A final Buffer pin retains the physical reader or writer row selected at admission. Each accepted
batch stores its actual serial. At Job completion or rejection of the suffix, the last confirmed serial on each
physical queue extends the pinned Buffer's frontier before release. This covers whole-buffer state transitions
even for disjoint byte ranges. An unknown submission contributes no serial.
The compiler merges adjacent or overlapping Buffer writes belonging to the same batch and condition.
Writes separated by a byte gap, a batch boundary or a condition retain separate invalidation ranges.
An offscreen image records producer use on the batches that reference or transition it. It becomes available as a new
publication after a selected write and acceptance of the complete Job.
For a current-run Target, acceptance of the complete producer Job releases its readers. Static imports use
their compiled first-use waits on each physical queue. Dynamic raster reads use their first selected graphics
batch. Both wait on the selected image's producer frontier, which includes boundary transitions. Producer
batches that never touch the image contribute no fence to this read. A previous Target publication
uses the selected image's availability and reader frontiers. Sampled offscreen Targets have one extra image
so the writer can preserve the last publication even with one frame in flight. The selected physical image
and its accepted uses are independent of the Job's FIF index. A final Buffer reader pins the exact published
writer, frame, FIF and range. NRI concurrent graph resources use semaphore memory dependencies and consumer-side
layout transitions. First and final Graphics boundaries cover preparation copies, History initialization and
canonical resource states.

An active submission borrows the Jobs frozen for that round until recording and native submission return.
Each accepted batch extends the receipt at its round ordinal and physical queue. The compiler lists the condition
regions that can write the Target in each batch. Each FIF updates the regions affected by changed inputs or History
validity and follows dependent binding choices in recording order. Toggling a region invalidates its command buffer. Dynamic Target reads that follow the installed Job order use it
directly. A reverse edge or self-read triggers a combined cycle check.
Acceptance evaluates output and History publication against that slot's frozen inputs.
The Server reads the receipt by ordinal and verifies the installed execution identity before publishing an output. Recording inputs, native command spans and fence arguments expire with the operation.
Each FIF slot caches its batch dependencies by the selected execution identities and logical edges. A changed
draw range patches the frozen operands. A new execution generation or a change of selected producer rebuilds
the topology. `dumpSubmissions` reports dependency rebuilds and native edges separately from accepted submissions.

With `LUX_RENDER_DEVELOPMENT_METRICS=ON`, `RenderServer::developmentStats()` exposes cumulative Job selection,
input rebuild and CPU round occupancy counters. The round borrows the installed product order, Target sources and
Resource identities until its submission terminates. Requests, cadence and completions only change the next
round's selection. Selection and topology growth counters track backing-array allocations in those two workspaces.
Slot hold time ends when the Server releases the round, before any remaining GPU retirement. The default build disables this sampling.

Ready batches are indexed by physical queue and wait condition. Accepting a submission advances its successors.
The compiler preserves physical-queue FIFO for object reuse and graph-owned memory aliasing. The Resource owner
qualifies final Buffer writer claims during installation and backing replacement. Disjoint writers can share a
native Buffer when their compiled uses preserve the same boundary on one physical queue. Two Jobs that require
separate transitions at a common boundary take turns under their product pins. Active writable allocations
belonging to unrelated Jobs must occupy disjoint memory.

A Job keeps a subscription key for each graph import that follows backing replacements. The Resource owns
the subscriber row and borrows its access contract from the immutable device graph. Final Buffer writers
and readers of a specific publication also have entries in the Job's product table. Admission pins their
selected backing, and submission records accepted queue use before releasing those pins. A reader of a
specific publication revalidates that same version at admission. An overwritten version fails the Run.

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
backwards distance together and creates a Previous/Current pair. `publishHistory(version, when)` observes the complete
resulting version on the selected path, including inherited mip ranges. A false publication condition holds the last
sample. A conditional final write with an explicit preserve fallback supplies its pass condition by default.
Distance counts backwards over successful publications. Each bank retains
the latest samples within that distance and allocates FIF plus distance physical slots.
Image initial values are explicit; Buffer history currently
starts at zero and requires a multiple-of-four byte size. The instance chooses a free write
slot by actual completion frontiers and publishes after every required batch is accepted. Complementary writers
can supply a complete sample on either path. Descriptor views are created at Start. Resize replaces only
History banks whose physical extent changed; other banks retain their allocation and invalidate their samples.
`InvalidateHistory` invalidates the selected attachment histories at the next preparation boundary.
An individual Previous read can name a compatible non-History resource as its first-sample fallback.
Analysis retains both inputs and their hazards; Prepare selects existing views or copy sources without copying
the fallback into a History slot. Different reads can declare different initial inputs. Fallback views are cached
with their frame bindings and change only when their physical input changes.

Resize preserves the installed Job identity, projection registrations, producer bindings and immutable device graph. Each Resize candidate belongs to its device and installed execution generation. Adoption checks live extents
before exchanging physical objects. A committed candidate is consumed. Compiled reverse indexes locate the
affected views, barriers, dispatches and copy layouts. A finite
`ResizeResources` candidate owns only replacement physical objects; adoption exchanges those objects without
recreating unrelated buffers or fixed-size images. Alias migration includes the connected closure of old and new
allocation groups. Actual placement still uses `instantiatePlacements` and the existing batch-dependency proof.

The Server captures the device-owned accepted frontiers of each affected Job execution at Resize admission.
An Offscreen Target adds the accepted uses of its image slots. Queue completion Senders wait for the merged
frontiers while the RenderThread continues other completion work.
Replaced objects use the sole `DeviceSession::cleanup_scope_`. WSI close retains its separate presentation
requirement; a failed replacement after closing the old SwapChain leaves an unavailable surface, with old output
borrows withdrawn. Resize replies describe adopted resources, not rendered pixels or completed presentation. Success pauses the
Target. Publish matching inputs and `SetTargetActive{target, true, epoch}` in the next complete frame; activation
checks the replied epoch so an older queued frame cannot resume resized output. A failed reply reports whether
physical output remains available, including an unavailable SwapChain after failed replacement.

`declareCondition` declares a dense Boolean input. `PassCondition` can also select History validity or the result of
`sameBinding(left, right)`. Sampled Target declarations keep a logical slot for each name. Cold Job installation
compares bound Target identities and installs the result in that Job. Frame conditions and History validity are
sampled during preparation. Compilation follows conditional writes and their predecessors by range and rejects a
read if a reachable path leaves any observed bytes or subresources undefined.
Use `builder.choose(predicate, when_true, when_false)` to return a resource version from each arm. Both callables run
while declaring the graph. An arm can return an existing input or record work and return the written version. A failed
arm rolls the declaration back. A consuming fixed pass or compute sequence is lowered for each operand selection,
with one logical write version for subsequent consumers. Explicit pass dependencies cover the alternatives.
Ordinary graph exports can observe either selected resource. A native Target export requires one writable destination.
For History, returning its Previous value on one branch holds the publication sequence on that branch. Returning
complete writes to Current on both branches produces a new sample on either path, subject to the publication condition
and submission receipt.
This authoring path currently accepts one controlling predicate for a specialized command or sequence. Independent
nested choices and draw-stream fan-out require additional lowering and linking support.
Consecutive passes sharing a predicate form one non-nested ConditionalRegion
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
FIF slot for an admitted frame and owns its accepted frontiers. Resource commands adopt CPU records on the
Server owner. Cold upload operations can collect their own accepted queue points. Factories receive the device,
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

The Server resolves running Jobs to a device-local list of installed Resources whenever the Job structure changes.
Frame algorithms read typed views in declaration order. The device owner selects each referenced Resource once
during synchronous input selection, even when several admitted Jobs read it. Different Resource identities
remain independent. Resources apply accepted copy receipts to the selected FIF replica.

A device round owns its frame slot from admission through recording and submission. The uploader owns its copy
commands and staging ranges. Once the queue accepts a window, the uploader commits its staging ranges and returns
the submission point and range count. The round uses that receipt to settle each participating Resource and
graph-buffer byte range. Cancellation during a native call joins its actual return before releasing the slot.
Accepted prefixes continue to protect their GPU objects. Frozen descriptor pools remain pinned through submission.

The compiled Pass offsets select one contiguous parameter region per FIF slice. Instance/default changes mark
only their compiled consumers. `ResourceInput::parameter_revision` is zero for a live external borrow, which
continues to be read each frame. A nonzero value promises unchanged bytes until `setResource` publishes a new
revision; immutable texture references and shadow-projection outputs use that contract. Address equality alone
never proves parameter contents unchanged. Default bytes are not recopied for a mapped-input-only change.

The device compiler emits the fixed pipeline, layout, descriptor-set and inline-constant bindings for each Pass.
Each physical CommandBuffer starts with unknown state, including the surface tail. At a conditional join the
compiler keeps facts shared by both paths. When paths leave different bindings, each FIF selects its compiled
exit delta when the predicate changes. Consecutive skipped regions inherit the last selected predecessor in that
command buffer. Buffer and image copies, uploads, readbacks and native zero fills preserve bindings across
the transfer pass. Dynamic Raster stages, attachment clears and storage fills reset the compiler's binding state. Consecutive Passes can
reuse shared sets, while independently writable private sets
and inline blocks select their own FIF operands. Dynamic Raster draws compare native bindings inside the stream.
The linker prepares a dense private descriptor layout for each program. Draw preparation qualifies array elements
against those fields before updating the native sets for the acquired FIF.

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
without consuming an index. An admitted frame holds the slot and its epoch through CPU preparation and submission.
Each accepted point advances that slot's frontier before the frame releases it. Partial submissions keep their
accepted prefixes after a failure. Background resource creation tracks its own GPU use.

## Output identity and swapchain flights

The Server stores Offscreen and Swapchain Targets as values. Each Target reserves its writer before asynchronous
attachment compilation and releases that reservation on the same operation's return. An existing writer's detach
only releases its own identity. Target operations check the writer and in-flight reads before changing the binding.

Offscreen owns the image, its views and the physical last-use frontiers. The device input table assigns one slot
per sampled Target and read mode. DrawList bindings share this slot across consumers in a round. Admission selects
the concrete image, sampled descriptor and queue frontiers on the Server owner. Resize preserves the format, sample
count and sampling support required by installed readers, including after their writer stops. Resize and removal
consult the active round before replacing a selected Target. The sole writer chooses an image
outside the current publication whose accepted uses have finished. Each accepted reader batch extends that image's
reuse frontier. Current-run readers depend on the producer's submissions. Readers of the latest publication
wait on the selected image's writer frontier. The Target publishes a new image after a complete submission that
wrote the output. Partial submissions, preparation-only submissions and disabled output passes preserve the previous image.

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

`JobCommands.hpp` exposes creation, start, stop, removal, cadence changes, exact execution
requests and Effect rebinding. `RenderServer` routes these commands to the Job and its current execution.

Start and running Rebind reserve a fresh execution generation before constructing their candidate. A failed candidate
consumes its generation and leaves the published Job, writer and dependency order intact. When the candidate is ready,
the owner installs the new generation, bindings and writer together. Stop closes admission, releases the writer and
cancels an unadmitted request. An admitted Run keeps its request until the actual submission result arrives. CPU
recording and host submission drain before the execution and its bindings are destroyed. A device keeps an outstanding
program compilation and its private candidates alive through driver return. Stop or replacement can release the original Job
execution during compilation. The owner checks the installed Job identity, execution generation and source revisions before
adopting the complete selection. A target Resize delays adoption until the target transaction has settled.
RemoveTarget returns Busy for an installed output or sampled input, including a stopped Job that is still draining.
A stopped Job keeps its logical Effect until the caller rebinds or removes the Job.

Only one exact request is pending per Job; a second receives Busy. A request arriving during an earlier continuous
Run belongs to the next Run. The terminal callback executes after the operation detaches all Job bookkeeping and
may release the Job. No Task/Future table or execution-pointer-based lifecycle inference is involved.

Start and Rebind sort installed Jobs by their current-run Target reads before publishing the candidate.
A cycle rejects that request and preserves the active binding. Jobs consuming an earlier Target or Buffer
publication use the selected backing and its GPU frontiers. Sharing a Storage introduces no Job ordering.
Each device reuses the installed order when admitting the ready Jobs for a frame.

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

Connect the `DeferredGeometryPorts::surface.semantic` bundle to `Lighting`, then connect the HDR output to `ToneMapping`.
The bundle carries albedo/metallic, normal/roughness, emissive/AO and forward-Z depth. GBuffer uses linear
RGBA8, two RGBA16F attachments and D32. Effect port semantics distinguish resources that share a format.

GBuffer receives the draw groups and visibility streams from MeshVisibility. Position, UV, world normal and tangent
requirements come from the chosen Cooked fragment's stage inputs. Lit geometry supplies normals; normal maps also
require a tangent including handedness. Nonuniform and mirrored transforms use inverse-transpose normals and
orthogonalized linear-transform tangents. Double-sided state and explicit front winding belong to the group/material.
AlphaMask uses the same Cooked coverage in GBuffer and Shadow; transparent BLEND is rejected by this path.

Direct lighting uploads each light array when it has a graph reader. A Shadow CPU view can select casters while
other light arrays retain their dirty ranges for a later GPU consumer.

The Scene owns one parameter row per View. Lighting, cluster construction and main-view frustum generation read its GPU projection. Mesh draw and shadow
CPU preparation borrow the logical row during frame selection. Mesh vertex shaders read the visibility pass's GPU matrix
for the selected layer. Each draw supplies a 32-bit layer index. Connect GBuffer and the optional Shadow node to the selected Lighting implementation,
then connect its HDR output to ToneMapping. The ToneMapping node owns exposure in EV.

`LightResource` supplies Scene-local directional, finite-range point and spot arrays. Client Commands
borrow unique input records through Response and return stable LightIds. Dense GPU arrays keep a GPU-format replica
source and dirty ranges for the actual FIF slices; removal updates the moved record's index. Upload and replica repair
use ResourceUploader. Unchanged frames do not create light Senders or scan all light records on CPU.

DeferredLighting remains the all-lights reference for Unlit and metallic-roughness PBR. With no shadow configuration
it compiles the unshadowed path, with no dummy map or sampler. It has no hidden ambient term; AO affects only the
explicitly supplied environment term. The SDR Tone Mapping implementations apply ACES. The sRGB implementation
encodes in the shader for UNORM targets and uses attachment encoding for sRGB targets. For linear FP16 offscreen
output, select `lux.render.tone_mapping.hdr_linear` and an RGBA16_SFLOAT target. It applies the same per-node
exposure in stops and preserves highlights above 1.0 for subsequent composition.

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

Shadow selects the Scene camera and the lighting Resource's caster records on the Server owner. When the camera
or caster revision changes, the projection writes the layer views, slice data, raster bias and dense light ranges
directly into its reusable output arrays. Shader inputs are frozen before recording. GPU passes perform culling
and rasterize current depth even when the CPU projection is reusable. Each installed Job has independent writable
shadow attachments for its FIF slots. GPU completion controls their physical retirement. Clustered lighting composes through the ports described below. EVSM, HZB occlusion,
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

Connect `DeferredLightingPorts::cluster_lists.semantic` to consume the resulting buffers. Check the status before indexing the list.
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

GBuffer and forward geometry declare their attachments and consume the shared camera visibility implementation.
A Compute pass derives the geometric view and clip planes from the Scene View GPU row. The Shadow implementation
declares its depth attachment and uses the projected light views for culling. The `frustum-culling`
option controls the bounds test. Each Job owns its visible members and indirect commands for each FIF slot.

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

Ordinary clients include `runtime/commands/ResourceCommands.hpp` and the `resource/<domain>/Resource.hpp` for the selected resource.
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

JobExecution supplies the frozen instances selected for the admitted device round.

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
DXIL and SPIR-V with canonical sets already at their final coordinates skip the relocation worker. Runtime validation uses pinned SPIRV-Tools
and the resolved SPIRV-Cross package; generation has no Engine toolchain or PATH/SDK fallback.

`ResourceParameter::setUniformConstant<T>(offset)` binds exactly one read-only uniform buffer block to the
specified graph buffer range. The compiler checks the complete `ConstantBlockTraits<T>` against the selected
target's reflected layout and the device's uniform buffer offset and range limits. The backend validates the
physical buffer and applies native CBV padding as needed.

An Effect state can expose a typed constant block and its nonzero 64-bit revision through
`EffectCompileContext::constantBuffer<T, State, ValueMember, RevisionMember>()`. Its command updates both
fields on the Server owner. The compiler qualifies the complete shader ABI and publishes a source slot
and the imported-buffer uses for each batch. Job installation binds both fields once. Jobs on one device share
the first selection made for a block in a round, including when backing capacity delays its upload.
The round retains selected CPU bytes for staging. Device-owned storage retains up to `FIF + 1` native
versions per block and reuses a version after its prepared readers, upload and accepted queue uses retire.

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
Registration also returns `indexed_mesh_shader`, indexed by `EMaterialPurpose`. The Material owner qualifies
both embedded vertex binaries against one vertex ABI. Each fragment input must match a vertex output by location
on SPIR-V or semantic and index on DXIL, with the same scalar and shape.
Each supported purpose owns its fragment stage and merged interface after registration. Both windings use the same
material records and vertex layout. Their vertex code and front-face state are selected separately. A rejected GBuffer
interface leaves a qualified Shadow interface available. These two requests prepare a clockwise GBuffer pass and a
clockwise layered Shadow pass for one material and mesh before issuing a Run.

```cpp
using Cold = lux::render::MaterialCommands::PrepareMeshPipelines;
using Purpose = lux::rdesc::EMaterialPurpose;
Cold::Request gbuffer{program, Purpose::GBUFFER, {{mesh, false}}};
Cold::Request shadow{program, Purpose::SHADOW, {{mesh, false, true}}};
```

Submit each request through the material command route. A successful reply covers native creation for both
windings. Forward uses `MeshPipelineUse::forward_format` to select the output attachment. Depth-only mesh
rendering uses Shadow with section bias disabled. The material owner matches requests and draws by the consumed
vertex layout, front face and complete pass target. Meshes with the same layout share the result.
Each request replaces the declared set for one material program and purpose. An empty list releases that set.
A failed replacement preserves the preceding publication. Replacing the same purpose while a request is active
returns Busy. A request for the currently published set revalidates its mesh identities and completes on the owner
while the compiler worker is busy with another program. A frozen draw keeps its native use through the accepted GPU frontier. On a later selection,
a missing compatible use returns `kMeshPipelineNotReady`. Prepare it and retry the Job with the same inputs.
Canonical empty records from constant or Shadow derivation are valid.

Default values do not distinguish table structure. Dynamic updates retain the table slot and do not compile
programs; a new target program creates a separate material identity. Native mesh variants are published on successful explicit preparation. Failed writes retain published CPU records and mark affected
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
transaction. Instance updates change draw group membership directly. The instance storage checks Mesh, material
and Transform identities before adopting the batch. At upload it resolves material GPU addresses for the selected
FIF slot. Clients release their draw references before releasing the associated Mesh or material.

Active graph consumers share each Resource's real FIF slices. Structural changes rebuild a list deduplicated by
ResourceInstanceId. Each resource tracks pending changes on its own physical replicas. A failed copy leaves
its changes pending. Accepted replacement buffers publish to all subscribers before subsequent use.
Create Scene transform records with `TransformCommands::Create` and set `MeshInstanceData::transform` to share them
with other Mesh instances in the same device and Scene. `TransformCommands::Update` and
`FrameData::appendData<TransformResource, TransformRecordUpdate>()` validate the whole batch before writing.
Rebinding or removing a Mesh instance releases its old reference. Releasing a referenced transform returns Busy.
TransformResource owns flat buffers or device-address pages per FIF replica. After the owner and generation check,
the GPU property stores a 32-bit TransformId at byte offset 20, equal to the validated TransformRef's local index on every Job and FIF. Paged shaders use its
upper bits to select a page and its low 14 bits to select the matrix. Staging detaches the selected dirty ranges
from later updates. Logical release invalidates the ID immediately. The same index can be issued to a new
generation after the active round closes and all accepted physical queue frontiers complete.
Releases with the same frontier share a retirement batch. The Transform owner reuses batch rows in FIFO order,
drops idle slot arrays above 4 KiB, and shrinks excess rows after a backlog subsides.

Update a material's reflected float4 color parameter through `MaterialCommands::UpdateColor` or
`FrameData::appendData<MaterialResource, MaterialColorUpdate>()`. Select the parameter index from the cooked program's
projection. The batch qualifies every record and field before adopting any values. The existing material page
supplies each FIF's upload, and the full material update command handles texture and structural changes.

Create a Scene and View, then send `UpdateView` with their `ViewScope` and a complete `ViewGpuParameters`.
The logical row exists before the first device or Job. A View FrameData column validates its batch and updates the same row.
On each rendering device, obtain the scoped projection using `GetOrCreateResource`. Bind
`ViewParameterResource::gpu_binding` for Lighting and main-view MeshVisibility. Scene pages hold 64 rows per FIF. The structured buffer view's
first element selects the requested row, so the shader reads element zero of that binding. The first GPU binding
allocates the backing. Removing a View immediately revokes its CPU identity. Its row index becomes available
for a new generation after the last accepted copy and graph use on each device has completed. An unrelated
removed row in the same page can be reused independently. A device with an unknown submission outcome retains
its affected rows until device retirement. Mesh visibility and shadow projection read the selected
`ViewGpuParameters` through the Resource's typed CPU view during frame preparation.
`SetToneMappingExposure` addresses a ToneMapping node by EffectId and NodeId. Compilation
validates the block's reflected ABI and links its writer. Before Freeze, a change in exposure updates the
writable FIF replica and invalidates the recording that references that block.

## CPU input columns

The destinations in a `FrameData` packet are Resource addresses. Each address combines a Server identity with
an instance generation. The packet retains its own typed vectors and borrows static channel metadata from the
Resource declaration. Keep the extension code loaded until its packets and accepted operations are destroyed.

```cpp
using ParticleUpdates = std::tuple<ParticleResource, ParticleResource>;
auto packet = FrameData::create<ParticleUpdates>({foreground.address(), trail.address()});
```

Batches follow the tuple order. For a repeated declaration use
`resolveColumn<ParticleResource, ParticleUpdate>(batch_index)` and append to the resulting typed column.
An unqualified column lookup requires a unique declaration and element type. Initial capacities are optional.
`reset()` clears element counts and preserves allocations. Finish writing through appended spans before moving the packet into a request.

The IPC request owns the typed columns. The update handler returns those columns in the response's `data` member,
including when resource validation fails. An explicit Run returns its ordered inputs in `JobSubmitted::updates`. Retrieve a packet by position with
`updates.packet(index)` and call `reset()` to reuse its column storage. A successful update acknowledges adoption.
A successful Run identifies the accepted native submission.

A frame fixes direct inline bytes and each shared Effect UBO revision at admission. A resident UBO version is pinned.
New bytes stay with the device round through backing or staging retries. Input commands from other connections
that write a resource still needed for preparation wait in the bounded input queue. This check follows the
installed resource dependencies and ends when the reader freezes its inputs. Resource removal consults the same
forward dependencies, including those of resources still being constructed.
`ListResourceJobs` accepts the address returned by `QueryResource` and lists the Jobs whose installed resource
lists contain that instance. `RemoveResource` takes the same address and applies the owning scope's removal
rules. On success a new instance of the same binding receives a new identity. A stopped Job with an unsettled execution keeps that relationship until its CPU
workspace and native bundle have released their borrows. Start and Rebind build the candidate resource list
on the installation stack. Installation also follows the construction dependencies once and keeps the indirect
IDs with that execution. Frame admission checks both sets for exclusive commands. On commit the candidate's
direct list swaps with the Job's list, protecting the outgoing resources while their
destructors run. Removal also checks in-flight commands, construction dependencies and the active installation.
A Resource's ID becomes inaccessible before its Storage destructor runs. Its dependency list remains registered
until that destructor returns. When a scope clears, all its IDs become inaccessible first and Storage destruction
then proceeds in reverse construction order. Successful zero-copy work leaves the current upload list as soon as
the attempted resource has received its settlement.

A Run carrying a packet leaves its updates pending until the device can use the selected FIF slot and output.
Before applying a window, admission follows installed `CURRENT_RUN` Target reads through their producers.
A missing producer rejects the window before its first update. A producer waiting for adoption of a separate
window returns `kJobBusy` to the dependent window. Runs that require one another's inputs can share one window.
An installed `LATEST_PUBLISHED` read checks its publication directly.
The server applies the Resource batches before reading the dynamic draw list for product ordering. Overlapping
draw binding ranges contribute each referenced ordinal once. Reverse current-run edges use the device's selected
product order again while the participating producer-reader relationships match. A later failure
returns the Run error. Earlier successful Resource batches keep their updates. Selected PER_RUN data belongs to that
frame, including when the draw list fails validation.

On the same connection, subsequent frame updates and mutable input commands wait for preceding explicit Runs to freeze
or settle. Adjacent Runs without new data can participate in the same device round. Stop can cancel a pending Run.
`RenderServerConfig::max_deferred_inputs` bounds deferred messages and Run windows awaiting freeze across all
connections (default 64). Each window occupies one entry regardless of its Job count. The slot is released when
all its Runs have frozen or settled. At capacity, a further waiting input receives `kRuntimeCapacity`. Independent
controls continue to dispatch.

### Submitting multiple Jobs with one set of updates

`RenderClient::submitWindow` takes the Jobs for one device, an ordered set of updates, and a reply handler.
For example, a scene Job and an overlay Job can consume the same View update and use private draw data from
the same publication.

```cpp
std::array jobs{scene_job, overlay_job};
WindowUpdates updates;
updates.append<UpdateView>(update_view_id, {view_scope, view_parameters});
updates.append(std::move(overlay_draw_packet));
auto published = client.submitWindow(jobs, updates, reply_handler);
```

Publication moves the updates into one IPC message. A transport rejection leaves them with the caller.
The server reserves each Job's pending request before adoption. Duplicate Jobs, Jobs on different devices,
and a busy member reject the window before it writes any inputs. Stopping a member before adoption
returns `kJobStopped` to each Run in that window. A native acquisition failure at this point returns
the acquisition error to each member. Server shutdown returns its cancellation or execution error to
pending Runs. Admitted Runs settle against their own device receipts.

A window accepts typed `FrameData` and synchronous View, Resource, or Effect input commands returning
`AckResponse`. The command ID belongs to the receiving Server. A request with borrowed fields requires its
source to stay readable through the lead Job's reply. Commands execute on the owner in append order. A failure
keeps earlier successful writes and terminates the Runs that required the remaining updates. Job-private PER_RUN
addresses must be bound to a participating Job. A shared PER_RUN resource is selected once for the device round.

Each Job has its own IPC correlation. The handler receives its `JobSubmitted` on the client thread when replies
are dispatched. The first listed Job's pending request owns the updates. The server borrows them during adoption.
That Job returns the storage in its terminal reply, including on cancellation.
Calling `clear()` on `reply.updates` retains its capacity for reuse. Individual
success identifies native acceptance after product publication. A failed suffix preserves the receipts of Jobs
whose submissions completed. Keep the handler context alive until every Job has received its terminal reply.

## Shared copy recording and preparation settlement

Graph transfers, staged asset/table uploads, texture clones and producer buffer preservation use the physical
recording functions in the private `device/CopyEncoding.hpp` header. Callers supply the exclusive command buffer and
qualified resource ranges. The shared texture encoder expands mip/layer regions; upload/readback retain the
qualified native row and slice layout. Graph compilation still owns graph barriers; Uploader owns its transfer
and queue-family handoffs. The encoders neither allocate resources nor submit or wait for GPU work.

Buffer upload segments use whole-buffer barriers. On D3D12 devices with legacy barriers, each upload segment
enters COPY_DESTINATION from COMMON and restores COMMON before the next segment. The graph compiler supplies
the known access at internal pass boundaries. On the same legacy devices a transient texture keeps its last state.
Each used subresource returns to COMMON after its final pass or enclosing render scope, before another alias takes over.
Subsequent runs can replay the same command buffer for that FIF. On legacy D3D12, a color resolve scope first
clears its actual single-sample destinations. This initializes placed render targets before the full-subresource resolve
and also works for per-layer draw sections. All submissions use SubmissionQueue and NativeExecution.

Each device round keeps one list of installed resource slots that still need uploads. Each attempted
resource receives its staged copy range and a settlement, including zero-copy attempts and failures after
a staged prefix. Returning false requests another attempt, for example when a replacement binding must
be published after native acceptance. Completed resources leave the list for the rest of the round.

FrameProgress adopts the accepted physical queue and serial before the resource settles its copy range.
Each resource owns its per-slot resident data and pending updates. Acceptance retires only the selected
changes. A rejected copy restores them alongside any changes made since selection.

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
bindings, pass inputs and History initialization commands for `FrozenRecordingFrame`. `FrameExecutor` borrows those
stable slices during synchronous recording; it has no GraphInstance access and performs no texture-description
queries. Changing author parameter bytes after preparation affects the next preparation, not this recording.
Physical binding or recording invalidation requires preparation again. The caller retains exclusive access to
the FIF data through recording and waits for accepted GPU uses before reusing its command allocators.

### Structural Resource commands

Mesh instance, light and material commands validate on the Render owner and adopt the logical change before
replying. Each affected device replica retains its dirty records until its safe frame prepares the upload.
Light removal moves the last dense record into the gap and updates the counts in the same owner call.
Cold Pipeline preparation and resource initialization may still suspend. An installed Resource that initializes a
new Buffer uses `ResourceUploader::writeBuffers` with an operation-owned `SubmissionFrontiers`. The frontier
records accepted work on each queue, including a successful Copy before a failed Acquire. Publish the
initialized backing with its dependencies or wait for that frontier before publication. Release old or failed
backings through the Device retirement path.

### C++ parameter limit

Render functions, constructors, factories, lambdas and callback contracts support at most four explicit parameters.
Use the domain value or the actual owner at the call site. For example, `StorageBindingOps::resolveGraphImport`
receives the Resource owner, projection index, graph description and destination binding span.

From the repository root, run the source signature check with Python and the pinned grammar packages:

```sh
python -m pip install tree-sitter==0.26.1 tree-sitter-cpp==0.23.4
python modules/function/render/test/CheckSignatureArity.py --base <commit-before-change>
python modules/function/render/test/CheckSignatureArity.py --base <construction-baseline> --revision <candidate>
python modules/function/render/test/CheckSignatureArity.py --full --revision <candidate> --manifest <file_actions.json> --report <evidence.json>
python modules/function/render/test/test_check_signature_arity.py
```

Diff mode compares tracked changes and untracked source files to the base (HEAD by default). Use the actual parent
for an incremental gate and the fixed construction baseline for a cumulative gate. Passing a revision reads the
candidate's Git blobs. Full mode examines all scoped sources, including existing declarations, definitions,
constructors, lambdas and function types. Pass the construction manifest to all three runs to include the named
consumers and planned paths. Additional repository paths can be selected with repeated `--include-path` arguments.

The report counts source occurrences. Separate declarations and definitions each count. In diff mode an identical
occurrence at the same path offsets one old occurrence; adding a duplicate, renaming a parameter, or moving a
nonconforming declaration to a new path requires review. Exit 1 reports over-limit signatures. Exit 2 reports
incomplete coverage, including parser errors or missing tokens, even when some violations could also be counted.
Review macro-generated and instantiated interfaces against the compiler or independent source evidence. Keep the
parser versions, actual file list, unresolved locations and findings from the JSON report with the build evidence.
