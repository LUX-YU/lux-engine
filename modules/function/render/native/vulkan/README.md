# Native Vulkan

Bootstrap owns `InstanceContext`. `DeviceSession` borrows it and owns one gapi LogicalDevice,
one immutable PhysicalDevice snapshot, the actually enabled features, VMA, physical queues,
upload facilities, a device descriptor array and retirement. Graphics/Compute/Transfer roles
may refer to the same physical queue; they never create duplicate submission serials.

## Resource ownership

VMA Buffer/Image are move-only owners. Their allocator must outlive them. Factories return unique
ownership; recording code borrows them. Image aliases share only their actual memory allocation.
Destroying either Image first is legal after GPU use ends. High-count CommandBuffer/Fence/DescriptorSet
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

MeshInstanceStorage has stable generational slots and one CPU Transform authority. Sparse updates
copy only unique dirty pages, using the Legacy 512-record page algorithm. GPU writes are ordered on
Graphics after old draws; CPU revisions commit only after submission admission. Requests supply
already unique changed instances. Growth allocates a replacement table; there are no triple mapped banks.
Each Context owns its derived Mesh selection and indirect Buffer. Storage only notifies matching
subscribers; selection changes are prepared at the next recording safe point. Existing recipes remain
limited to their declared record prefix.

MeshStorage uses chained Buffer arena segments for combined vertex/index ranges. MaterialStorage
uses an arena per complete Cooked program/layout. VMA virtual blocks reuse free ranges; physical
Buffers have one owner. A retiring range retains only virtual allocator metadata, allowing its delayed
free after a closing Storage has retired the segment. It does not retain Device, Storage or Buffer.
Range uploads use the existing UploadContext on Graphics, avoiding ownership transfers of neighboring
live ranges. Standalone Texture uploads still select the actual dedicated/fallback queue path.

Material parameter and texture counts come from the cooked program. ShaderEmitter and runtime use
that same variable record shape; old incompatible material assets must be cooked again. Material
holds logical TextureId references. Texture changes invalidate only actual consumers; missing or stale
references sample the device's real white texture. Old Image/descriptor contents survive only prior GPU
use, not a Material's future reference. Parameter-only changes do not compile another Pipeline.

## Completion and shutdown

Queue poll observes native progress. `dispatchCompletions()` is the Owner safe point: it unlinks each
callback before invoking it, then collects retired objects. A callback may end its own operation state.
Only the host runs threads; Native does not add a worker or a second scheduler.

Stop admission, request stop and settle CPU/Owner tasks, while continuing to advance GPU completion.
Scope join does not replace producer-tail or host join. End all Contexts and logical Storage owners,
drain retirement, then destroy device facilities, LogicalDevice and finally bootstrap InstanceContext.
Standalone Native consumers must obey the same order. `DeviceSession::drain()` waits already accepted
submissions; it is not a join of external producers which could submit new work afterward.
