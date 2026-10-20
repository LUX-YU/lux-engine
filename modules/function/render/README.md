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

Built-in registration definitions are compiled in their functional implementation files. Applications do not
construct a Provider or include a concrete Storage. Project extensions submit their factories and typed Handler
bindings through the same registration transaction, without changing Server dispatch branches.

## Requests and execution

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
Logical lifetime disjointness does not authorize physical memory aliasing or implement runtime history rotation.

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

`RenderExecutor` records work into FrameExecution's existing shared Graphics submission. Professional preparation
may contribute bounded Buffer copies; it cannot hide arbitrary GPU dispatch behind a recording callback.
Graph async compute, History rotation and memory aliasing are not enabled. Explicit Buffer/Texture copies are
available; there is no production Readback service or implicit extra rendering.

`dumpRenderGraph` and the project-side `dumpRenderPlan` synchronously emit JSON or DOT into a borrowed sink.
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
Cooked material shader ABI 2 and asset payload version 8 encode this contract; older artifacts require recooking.

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

The Runtime rebuilds its non-owning Storage preparation list when Attachment membership or Context identity
changes. Project GPU consumers likewise call `FrameExecution::collectStorageUses` at those changes and supply
the resulting span to `FrameExecution::render`. Preparation callbacks are synchronous: they do not submit, suspend or dispatch
completion callbacks. Stable frames neither rebuild this list nor sort Storage relationships.

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
