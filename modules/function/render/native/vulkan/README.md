# Native Vulkan

Bootstrap owns `InstanceContext`. `DeviceSession` borrows it and owns one gapi LogicalDevice,
one immutable PhysicalDevice snapshot, the actually enabled features, VMA, physical queues,
upload facilities, a device descriptor array and retirement. Graphics/Compute/Transfer roles
may refer to the same physical queue; they never create duplicate submission serials.

## Compilation inputs

`indexPrograms(definition, programs)` resolves author names once and returns borrowed pointers in
GraphPassId order. `compileGraph` receives that index, the same Definition/Analysis, actual PhysicalDevice,
VkDevice, enabled features, queue flags, sampled-image capacity and qualified external Storage declarations.
It neither owns nor queries DeviceSession/Registry. The caller keeps program input and the LogicalDevice
alive through compilation; the resulting plan owns its own pipelines and data. A standalone consumer
qualifies Storage factories at its cold installation/assembly boundary before compiling.

## Resource ownership

VMA Buffer/Image are move-only owners. Their allocator must outlive them. Factories return unique
ownership; recording code borrows them. Each Image owns its paired VMA allocation; no Image Alias API
is exposed. High-count CommandBuffer/Fence/DescriptorSet
and Pipeline handles remain carriers owned by their Queue, Context or compiled-plan collection.

StorageSet exclusively owns professional Storage instances. ResourceRegistry holds non-owning indexes.
Effect owns its immutable CompiledGraph; Attachment owns ExecutionContext. Context owns its frame
resources and borrows the plan. End subscriptions before destroying Storage, and end Context borrows
before releasing its plan. Clients settle related operations and detach uses before deleting Storage.
No shared Context/Storage owner repairs an invalid call order.

Physical replacement or release moves ownership into the Session's retirement facility. Reserve the
retirement record before publishing or submitting a physical resource. Transfer captures each physical
queue's already accepted progress once; future work does not extend it. Pools can keep capacity after
logical deletion. The old range or descriptor slot is reusable only after its accepted GPU use ends.
Staging retirement depends only on the queue which consumed its copy. There is no outward Ticket,
per-resource LastUse or batch-owned resource reference list.

## Upload and rendering

Professional storage returns its upload Sender. `render::upload(storage, request)` preserves that
concrete Sender. The operation owns input and unpublished content; UploadContext only stages bytes,
records copies and establishes queue dependencies. CPU preparation uses the host CpuScheduler and
returns to the Owner scheduler. Begin/Record/Submit is one non-suspending segment.

Successful upload means required Copy/Acquire submissions were accepted, future use is ordered, and
Storage published the handle. It does not require waiting for the last GPU fence. Failure retains
last-good published content; accepted physical work still retires safely. An uncertain accepted side
effect is reported as `kOutcomeUnknown`, never as a guaranteed rollback.

Prepare changed bindings at the Owner safe point. Stable frames use prepared handles, offsets and
parameters, without Feature callbacks, name lookup, Shader compilation or Storage-wide polling.
Descriptor sets are not rewritten while that Context's previous submission is active. A Context
currently has one in-flight frame resource set; queue capacity is not a claim of FIF support.

There is no production Readback Command or automatic output-to-host allocation. Ordinary graph Copy,
Native mapping and cache maintenance remain available. External verification explicitly copies output
and waits for completion; reading does not implicitly execute another frame.

## Professional data

MeshInstanceStorage has stable generational slots and one CPU authority for Transform, properties
and alive fields. Storage pages contain 16,384 slots; sparse uploads merge only dirty 512-slot blocks.
An enabled BDA device uses a two-level page-address table; the fallback grows continuous field Buffers.
GPU writes are ordered on Graphics after old draws; CPU revisions commit only after submission admission.
Requests supply already unique changed instances. There are no triple mapped banks or draw-prefix
capacity limits. Each Context owns its Feature-derived Mesh selection and indirect Buffer. Storage
reports structural changes; the Feature decides membership and prepares changed data before recording.

MeshStorage uses separate chained vertex/index arenas and a compact GPU section table. The public
`MeshGpuSection` contains geometry facts only. The Mesh Feature copies these fields into its own
indirect commands and supplies instance selection/count itself. VMA virtual blocks reuse free ranges;
physical Buffers have one owner. A retiring range borrows its segment's virtual allocator metadata.
Closing an arena first removes those metadata-only retirement records, then retires the entire segment
against accepted queue progress. It does not keep a shared Device, Storage or Buffer owner.
Range uploads use the existing UploadContext on Graphics, avoiding ownership transfers of neighboring
live ranges. Standalone Texture uploads still select the actual dedicated/fallback queue path.

MaterialStorage owns a continuous parameter table for each complete Cooked Shader/layout, with stable
local GPU slots. Values belong to Material writes, not program identity. Offsets and record stride come
from the Shader's reflected Cooked metadata; runtime does not recompute the ABI from parameter counts.
Material assets without that layout require re-cooking. Material holds logical TextureId references.
Texture replacement records all affected Material integer-field updates in one Graphics batch, using
each MaterialStorage's recorder. Submit success adopts those CPU fields and the Texture together;
rejection retains the old published values. A Material upload that suspended re-resolves its Texture
references at adoption. Missing or stale references sample the device's white texture. Old physical
content survives prior GPU use only. Parameter-only changes do not compile another Pipeline.

Attach writes borrowed Buffer/parameter facts directly into the Context's resource records. Intrusive
non-owning subscriptions mark only affected descriptor, barrier and parameter locations. Storage owns
professional updates; Context never carries a Storage upload task or writes Material data on its behalf.

## Completion and shutdown

Queue poll observes native progress. `dispatchCompletions()` is the Owner safe point: it unlinks each
callback before invoking it, then collects retired objects. A callback may end its own operation state.
Only the host runs threads; Native does not add a worker or a second scheduler.

Stop admission, request stop and settle CPU/Owner tasks, while continuing to advance GPU completion.
Scope join does not replace producer-tail or host join. End all Contexts and logical Storage owners,
drain retirement, then destroy device facilities, LogicalDevice and finally bootstrap InstanceContext.
Standalone Native consumers must obey the same order. `DeviceSession::drain()` waits already accepted
submissions; it is not a join of external producers which could submit new work afterward.
