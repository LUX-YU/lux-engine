# Render

Render is one shared library. Applications link `lux::engine::function::render` from
`find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render)`. Loading the library does not create a GPU
device. Backend resources and implementation dependencies stay inside the library.

The backend is pinned NRI v180 with Vulkan selected at build time. NRI owns the native device and physical
allocations; Render owns its submission, upload and frame policies. NRI and Vulkan types appear only in project
extension or private headers. Checked backend failures remain structured Render errors.

## Public and project contracts

- `include/` contains installed values, Commands, graph declarations, descriptors and Server APIs.
- `sinclude/` contains uninstalled project extension contracts for device access, uploads, Storage factories,
  Feature definitions and generic registration. A GPU extension explicitly selects these dependencies.
- `pinclude/` contains concrete built-in Storage/Handler implementations and private algorithms.
- `src/` follows the same responsibilities. A single target compiles each production source once.

Public headers contain no graphics API types. Neutral Command and resource declarations do not require stdexec;
the Server's actual asynchronous Handler interface does. Project extension headers are a source integration
contract for the selected backend and compiler ABI, not a separately installed graphics SDK.

Applications explicitly register descriptions:

```cpp
#include <lux/engine/render/features/MaterialMeshRaster.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>

auto server = lux::render::createRenderServer(config, execution, options);
if (!server)
{
    return server.error();
}
auto feature = (*server)->registerFeature<lux::render::MaterialMeshRasterDescriptor>();
```

`IndexedTriangleMeshStorageDescriptor`, `RasterMeshInstanceStorageDescriptor`,
`RasterMaterialTableStorageDescriptor` and `SampledTexture2DStorageDescriptor` describe distinct data and binding
contracts. Their descriptions explain intended use; description text does not participate in identity or ABI
comparison. Different contracts use different semantic names. Instance keys only select instances of one type.
`ListFeatures` and `ListStorages` return independently owned names and descriptions.

`FeatureDescriptor::describe_ports` exposes config-dependent port information without building a graph or creating
device objects. Composition calls that same cold entry. Imported ports carry their initial use; selected exports
carry their exact range and final use. Concrete graph building remains in the project-level Feature implementation.

Built-in registration definitions are compiled in their functional implementation files. Applications do not
construct a Provider or include a concrete Storage. Project extensions submit their factories and typed Handler
bindings through the same registration transaction, without changing Server dispatch branches.

## Requests and execution

An Attachment owns its professional Feature bindings and Storage subscriptions. The graph instance receives borrowed
`ResourceInput` values; it does not discover Storage, own Feature objects or call their preparation functions.
FrameExecution prepares the Attachment's borrowed work at the reusable FIF slot. ResourceUploader records its
preparation copies before RenderExecutor records graph work. Detach and replacement disconnect the professional
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

Synchronous Handlers fill the Response directly in the Outbox message. Asynchronous Handlers return a concrete
Sender. `counting_scope + spawn` owns connected operation states; a `ResponseDelivery` only carries the completed
result and routing through the existing RunLoop to `ResponseOutbox`. The RenderThread publishes IPC replies.
The CommandRegistry owns one Command directory, stable Handler objects and one selected binding index per
Command. Installing a batch prepares and validates all its entries before publishing. Selection affects future
dispatch, never an operation already started with another Handler.

Upload sources are borrowed. After successful submission, the Client keeps their addresses, extents and contents
valid until Response, including failure or cancellation. Failed submission takes no borrow. Abandon or connection
loss does not prove reading ended; the application settles the Server and CPU producers before releasing such
inputs. Native staging and submitted GPU resources remain the device's responsibility.

`ConfigValue` exclusively owns one immutable typed configuration. Its defining code must remain loaded through
destruction. The compile request owns configurations during asynchronous compilation; graph assembly borrows them
synchronously. Dynamic configuration cost counts actual owned capacity, not external upload bytes.

## Graph and Features

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

`FeatureDefinition` holds project-side describe/build callbacks. The public FeatureDescriptor only describes
identity/configuration and purpose. `MaterialMeshRasterDescriptor` and `MeshDepthRasterDescriptor` share the
documented raster mesh parameter ABI; their implementation and dependency lists remain private. Features describe
draws, compilation lowers them, and execution records them. No Feature build callback or graph analysis runs in a
stable frame. Cooked shader reflection remains the source of parameter layouts.

`GraphCompiler::compile` consumes the immutable graph and its analysis, without a Storage registry or Feature
factory. `PipelineLayoutPlan` determines SPIR-V relocation, PipelineLayout and Descriptor writes together.
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

`RenderExecutor` records work into FrameExecution's prepared command buffers. Professional preparation
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
does not publish a sample. Descriptor views are created at Attach, not on each rotation. Resize creates a fresh
candidate; `InvalidateHistory` invalidates the selected attachment histories at the next preparation boundary.
An individual Previous read can name a compatible non-History resource as its first-sample fallback.
Analysis retains both inputs and their hazards; Prepare selects existing views or copy sources without copying
the fallback into a History slot. Different reads can declare different initial inputs. Fallback views are cached
with their frame bindings and change only when their physical input changes.

`declareCondition` declares a dense Boolean input; `PassCondition` selects that input or History validity.
Attachment conditions are zero/one bytes and are sampled once during preparation. Conditional writes require
an available predecessor fallback. Consecutive passes sharing a predicate form one non-nested ConditionalRegion
and sample it once. Both paths record the same compiled transitions; the false path preserves predecessor content.
Rendering groups may merge within a region but never across its boundary. There are no predicate callbacks or
GPU-to-CPU reads.

`HzbFeatureDescriptor` consumes forward-Z depth and produces current/previous R32 depth pyramids. It ports Legacy's
per-mip maximum reduction with complete odd-size footprint coverage. History, initialization and barriers belong to
the graph instance and plan; the feature has no private double-buffer or barrier owner. `ImageDispatchArguments`
resolves group counts from the actual attached mip extent, including after Resize.

`dumpGraph` and the project-side `dumpPlan` synchronously emit JSON or DOT into a borrowed sink.
They do not open files, wait for the GPU or trace stable frames. Compiled plan counts are distinct from measured
execution counters.

## Resources and frame lifetime

DeviceSession contains the device, immutable capability facts, unique physical Queue collection, shared
DescriptorTable and `cleanup_scope_`. The selected backend's extension table is private to its implementation.
ResourceUploader owns staging and upload batches. FrameExecution owns the frame slots and preparation/submission
phases. Runtime owns ResourceRegistry and the Global StorageSet; factories synchronously borrow an explicit
StorageCreateContext. StorageSet owns professional instances; ResourceRegistry borrows them for generational lookup. Effect owns its
compiled graph. Attachment owns the execution instance and borrows the Target's output. The Client settles uploads
and detaches consumers before deleting Storage; arbitrary early deletion is not a cancellation API.

DescriptorTable maintains independent image and sampler indices. Identical sampler descriptions reuse one
device-lifetime sampler. Texture references carry both indices and explicit flags; the material sampling code
returns white for its default flag without accessing a descriptor. The device does not allocate a white texture.
Cooked material shader ABI 4 and asset payload version 9 encode this contract; older artifacts require recooking.

The default is two frames in flight, configurable from one to three. Dynamic instance fields, page tables,
material parameters, descriptor records, indirect data and Target outputs have physical frame slices. CPU business
data and immutable programs remain single authorities. A frame slot is reusable only after its CPU writers end
and every actual participating physical Queue completes.

Storage upload Senders use the existing CPU scheduler and RenderThread resumption. ResourceUploader packs finite
staging windows and batches Copy work. Frame preparation contributes missing ranges before compatible graph
recordings enter the shared submission. Dirty-word batching and contiguous Transform input avoid per-element Copy
construction. There is no per-frame Sender for unchanged drawing and no unconditional sleep in the event loop.

TransformBatch borrows separate key and matrix spans with equal lengths and unique, current keys. The Client
retains both spans through Response and supplies finite matrices. Create/Update instance arrays follow the same
borrow rule. Storage validates identities at admission and retains one GPU-format copy of adopted data to fill
older FIF slices. This is a replica source, not another authoritative scene model. A successful upload adopts
the new CPU values only after all necessary submissions are accepted. A partial failure keeps the previous
adopted values and marks affected GPU ranges for repair before another Draw. Client buffers may then be reused.
Buffer growth keeps the previously published binding until the complete candidate is accepted; failed growth
cannot expose a half-populated buffer. Repair failure stops rendering instead of drawing mixed revisions.

FIF replica preparation coalesces adjacent dirty words before splitting at physical pages and finite staging
windows. Submission acceptance acknowledges only copied records; failed windows remain dirty. CPU preparation
and descriptor updates finish before `SubmissionQueue::recordAndSubmit` begins recording.

The Runtime rebuilds its non-owning Storage preparation list when Attachment membership or RenderGraphInstance identity
changes. Project GPU consumers collect the actual `StorageFrameUse` borrows at those changes and supply their span
to `FrameExecution::render`; each entry names its storage and `StorageBindingOps`. Preparation callbacks are synchronous:
they do not submit, suspend or dispatch completion callbacks. Stable frames neither rebuild this list nor sort
Storage relationships.

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

Install `GBufferFeatureDescriptor`, `DeferredLightingFeatureDescriptor` and `TonemapFeatureDescriptor` independently.
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

`DirectLightingStorageDescriptor` supplies Scene-local directional, finite-range point and spot arrays. Client Commands
borrow unique input records through Response and return stable LightIds. Dense GPU arrays keep a GPU-format replica
source and dirty ranges for the actual FIF slices; removal updates the moved record's index. Upload and replica repair
use ResourceUploader and FrameExecution. Unchanged frames do not create light Senders or scan all light records on CPU.

DeferredLighting remains the all-lights reference for Unlit and metallic-roughness PBR. With no shadow configuration
it compiles the unshadowed path, with no dummy map or sampler. It has no hidden ambient term; AO affects only the
explicitly supplied environment term. Tonemap applies ACES and an explicit linear/sRGB output choice.

## Shadow composition

Install `ShadowMapFeatureDescriptor`, `MeshVisibilityFeatureDescriptor` and `MeshShadowFeatureDescriptor` independently.
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

Install `ClusteredLightingFeatureDescriptor` independently. It contributes Reset/Build/Count/Scan/Fill graph work;
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

`MeshInstanceDrawGroup::material` selects a material **program table** through a live material in that table;
it is not a filter for just that MaterialId. A group selects its Mesh/submesh and all matching instances in that
table. Each instance's GPU material index selects its own parameters. Configure one group for that Mesh/submesh/table,
even when many MaterialIds share it. Listing those materials as separate groups repeats the same draws.

Shadow visibility can supply external visible members, indirect commands and counts to MeshShadow. GBuffer currently
uses its own all-candidate member stream; it does not yet expose the equivalent external visibility inputs. A passing
shadow-culling test therefore does not establish a GPU-culling main-view path.

Measure the actual composition: output extent, camera coverage, active and shadow-casting light counts, shadow slices,
resolution/filtering, view count, FIF and update cadence. An offscreen Render measurement excludes presentation and the
rest of the application. All-lights and Clustered modes can be compared with the same scene; no light-count threshold
or automatic mode switch is implied. Overflow fallback must be distinguished from a successfully built list.

Production progress counts accepted executions. Steady-state throughput over many bounded FIF cycles is useful, but
it is not a measurement of individual GPU completion or display latency. CPU record/submit time, GPU timestamps,
Client response observation and profiler sampling remain separate measurements. In particular, a test Client's reply
polling or sleep time is not RenderThread work. A frame-driven Client can keep one borrowed update batch in flight and
consume its response on later ticks before reusing that input.
