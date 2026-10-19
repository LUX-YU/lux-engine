# Native Vulkan

Bootstrap owns `InstanceContext`. `DeviceSession` borrows it and owns one gapi LogicalDevice,
one immutable PhysicalDevice snapshot, the actually enabled features, VMA, physical queues,
upload facilities, a device descriptor array and one physical cleanup scope. Graphics/Compute/Transfer roles
may refer to the same physical queue; they never create duplicate submission serials.

## Compilation inputs

`indexPrograms(definition, programs)` resolves author names once and returns borrowed pointers in
GraphPassId order. `compileGraph` receives that index, the same Definition/Analysis, actual PhysicalDevice,
VkDevice, enabled features, queue flags, sampled-image capacity and qualified external Storage declarations.
It neither owns nor queries DeviceSession/Registry. The caller keeps program input and the LogicalDevice
alive through compilation; the resulting plan owns its own pipelines and data. A standalone consumer
qualifies Storage factories at its cold installation/assembly boundary before compiling.

## Resource ownership

VMA Buffer/Image are lightweight move-only gapi carriers: handle, allocation and necessary shape.
They contain no Device/Allocator and perform no implicit destruction. Factories return values;
the actual Storage/Context/Arena collection releases them with its Session allocator. Release an
occupied carrier before move assignment. No Image Alias API is exposed. CommandBuffer/Fence/DescriptorSet
and Pipeline handles remain carriers owned by their Queue, Context or compiled-plan collection.

StorageSet exclusively owns professional Storage instances. ResourceRegistry holds non-owning indexes.
Effect owns its immutable CompiledGraph; Attachment owns ExecutionContext. OffscreenTarget owns the
actual output Image/View; Context borrows that output and owns the other frame resources. End
subscriptions before destroying Storage, and end Context borrows
before releasing its plan. Clients settle related operations and detach uses before deleting Storage.
No shared Context/Storage owner repairs an invalid call order.

Physical replacement or release transfers the value into `DeviceSession::deferRelease`, whose operation
state belongs to stdexec::spawn and the Session's single simple_counting_scope. All physical queue
conditions are captured and registered in the same Owner segment; future work does not extend them.
Callbacks are ordered by serial and then registration order, including already completed conditions.
Final release runs directly in the last callback. An Arena has no local scope, counter or owner table.
The old range or descriptor slot is reusable only after accepted GPU use ends. Staging page return waits
only for its actual Copy queue. There is no outward Ticket, per-resource LastUse or batch resource list.

gapi remains header-only. Native enables `LUX_GAPI_ENABLE_VMA` and exports its VMA header dependency;
`src/device/Vma.cpp` is the sole VMA implementation. Plain Vulkan and VMA carriers use distinct types
and paired release APIs. Host mapping addresses live in the Staging page collection, not every Buffer.

## Upload and rendering

Professional storage returns its upload Sender. `render::upload(storage, request)` preserves that
concrete Sender. The operation owns input and unpublished content; UploadContext only stages bytes,
records copies and establishes queue dependencies. CPU preparation uses the host CpuScheduler and
returns to the Owner scheduler. Begin/Record/Submit is one non-suspending segment.

`writeBuffers` accepts multiple borrowed ranges in one operation. UploadContext packs a finite window
of Staging ranges with one CPU scheduling step and combines ready work into physical queue submissions.
Adjacent Buffer ranges gather directly from the Request into Staging without an intermediate GPU
update Buffer. Required fills use that same submission path. Pages
reuse their bytes only after CPU reservations and their actual queue conditions end. Counters distinguish
CPU preparation windows, Copy regions, Copy submissions and Acquire submissions; they are not synonyms.

DeviceSession selects the writable frame and closes a bounded input prefix. Dynamic Storage operations
join that frame before their first write, then adopt or roll back before ending their participation.
After Queue observation and dispatch, the host consumes the Owner ready prefix before calling `render`.
This lets a woken writer join before the next frame closes admission. `render` also seals upload-only
frames when there are no active Contexts. Once writes settle, Contexts prepare bindings and record into
one shared batch; only the Session submits it. Contexts do not submit independently. The default is
two frames in flight, configurable from one to three independently of Queue capacity. Instance fields,
page tables, Material parameters, Context temporaries/descriptors, indirect data and Target outputs
have corresponding physical slices. CPU business data, compiled indexes, Shaders and Pipelines remain
single authorities. A slot is reusable only after every actual participating Queue completes and all
CPU writers have ended.

Published-data preparation contributes missing ranges synchronously to the same UploadContext. A finite
window can submit copies alone and retain the PREPARING phase; the final window and compatible Views
record in one batch. `render()` returns the number of actual submissions, including copy-only progress;
Context execution counters distinguish completed recording of a graph. No per-frame Sender is created.
With multiple Staging pages, active rendering reserves one existing page from background admission.
A one-page configuration serializes reuse and does not provide this independent preparation capacity.

Successful upload means required Copy/Acquire submissions were accepted, future use is ordered, and
Storage published the handle. It does not require waiting for the last GPU fence. Failure retains
last-good published content; accepted physical work still retires safely. An uncertain accepted side
effect is reported as `kOutcomeUnknown`, never as a guaranteed rollback.

Prepare changed bindings at the Owner safe point. Stable frames use prepared handles, offsets and
parameters, without Feature callbacks, name lookup, Shader compilation or Storage-wide polling.
Descriptor sets are rewritten only for the selected reusable slice. `nativeBuffer(resource, slot)` is
a diagnostic physical borrow, not a readiness check. A consumer must prepare/submit that slot and observe
real completion before host reading; the last-submitted slot index alone does not prove completion.

There is no production Readback Command or automatic output-to-host allocation. Ordinary graph Copy,
Native mapping and cache maintenance remain available. External verification explicitly copies output
and waits for completion; reading does not implicitly execute another frame.

## Professional data

MeshInstanceStorage has stable generational slots and one CPU authority for Transform, properties
and alive fields. Storage pages contain 16,384 slots. A 512-slot indexing block orders only changed
Request entries; holes are not uploaded. `CreateMeshInstances` validates a whole batch, uploads its
fields through one operation and publishes IDs together in input order. The single-create API uses
the same algorithm. An enabled BDA device uses a two-level page-address table per slice; the fallback
grows continuous field Buffers per slice. GPU writes target a free slice on Graphics; CPU fields commit
only after all required windows are accepted. A failed partial write marks affected fields for repair
from the unchanged CPU authority before the next Draw.
Requests supply already unique changed instances. There are no triple mapped banks or draw-prefix
capacity limits. Each Context owns its Feature-derived Mesh selection and indirect Buffer. Storage
reports structural changes; the Feature decides membership and prepares changed data before recording.
Dirty bitmaps track missing records separately for each physical slice. Bounded selections acknowledge
only successfully submitted masks, with O(1) nonzero-word removal. Zero change does not scan live records.
Only first-slice preparation and actual growth rebuild complete ranges; normal updates copy K changed
fields from the Request to Staging and then adopt those K values into the CPU pages.

MeshStorage uses separate chained vertex/index arenas and a compact GPU section table. The public
`MeshGpuSection` contains geometry facts only. The Mesh Feature copies these fields into its own
indirect commands and supplies instance selection/count itself. VMA virtual blocks reuse free ranges;
physical Buffers have one owner. A retiring range borrows its segment's virtual allocator metadata.
Closing an arena registers all remaining range callbacks before its final segment cleanup. Equal-serial
FIFO on each physical queue and immediate range return ensure metadata remains valid until all borrowers
finish. Segment addresses remain stable during ownership transfer. No queue-node removal or local join
is needed, and no shared Device, Storage or Buffer owner is retained.
Range uploads use the existing UploadContext on Graphics, avoiding ownership transfers of neighboring
live ranges. Standalone Texture uploads still select the actual dedicated/fallback queue path.

MaterialStorage owns a continuous parameter table for each complete Cooked Shader/layout, with stable
local GPU slots. Values belong to Material writes, not program identity. Offsets and record stride come
from the Shader's reflected Cooked metadata; runtime does not recompute the ABI from parameter counts.
Material assets without that layout require re-cooking. `CreateTexture` and `CreatePatchedTexture` return
new immutable identities; neither changes another Storage. Clients issue `UpdateMaterial` to replace
parameters or Texture references, then release the previous Texture after its references have ended.
The update keeps the Material's Cooked program and GPU slot. Missing or stale Texture inputs resolve
to white at this explicit boundary; an invalid later release is not repaired automatically.
`UpdateMeshInstances` changes Mesh/Material references explicitly. Clients sequence related operations
and coordinate multiple connections. There is no business revision CAS or Query mirror of Client input.
Old physical content survives accepted GPU use through Session cleanup. Parameter-only changes do not
compile another Pipeline; internal subscriptions report real buffer growth to attached Contexts.
Ordinary Material updates write the selected free table slice directly. Other slices retain sparse dirty
record bits; only tables with pending work enter preparation. Large records can span finite windows without
acknowledging the record early. The committed parameter bytes remain
the sole CPU authority; failed preparation never publishes scratch content. Attach checks the complete
reflected record ABI, including member types, offsets and strides, without retaining another Shader copy
per texture port. Mesh bindings carry only vertex/index ABI requirements, not source byte offsets or
complete asset descriptions. Feature configuration owns the one cold geometry/program input used to compile.

Attach writes borrowed Buffer/parameter facts directly into the Context's resource records. Intrusive
non-owning subscriptions mark only affected descriptor, barrier and parameter locations. Storage owns
professional updates; Context never carries a Storage upload task or writes Material data on its behalf.

## Completion and shutdown

Queue poll observes native progress. `dispatchCompletions()` is the Owner safe point: it unlinks each
callback before invoking it. A callback may end its own operation state; the queue never touches it afterward.
Only the host runs threads; Native does not add a worker or a second scheduler.

Stop admission, request stop and settle CPU/Owner tasks, while continuing to advance GPU completion.
Scope join does not replace producer-tail or host join. End Contexts, logical Storage owners and compiled
plans, transferring their physical contents while cleanup admission is open. Final `DeviceSession::close()`
clears Global Storage, closes the cleanup scope and advances queues until join. DescriptorArray, Staging
pages and Upload timestamp data remain alive until then; final cleanup never spawns another cleanup.
Queue/Allocator/LogicalDevice and finally bootstrap InstanceContext are released afterward.
Standalone consumers obey the same order. `drain()` waits accepted submissions and dispatches completion;
it does not close the scope or join external producers. Upload and execution can continue after drain.
Extension hosts keep cleanup/destructor code loaded through final Session close and producer tails;
destroying a Handler is not permission to unload code still used by its transferred physical resources.
