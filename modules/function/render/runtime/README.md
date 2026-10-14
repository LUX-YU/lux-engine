# Minimal Render runtime

`render_client` has only Core/Data/Protocol and Platform IPC dependencies (the completion value owns its IPC reply). It does not include
or link Native. `render_runtime` supplies the actual Vulkan-backed server, assembled on a host-owned
thread. Its public header is neutral. There is no additional Endpoint, dispatcher or object registry.

The server owns Scene/Target/Effect SlotMaps. Each Scene owns its View SlotMap. An Effect owns its one
immutable compiled graph. Each client owns its connection-local RequestId SlotMap. The server owns
the frozen OperationId SlotMap of descriptor/handler/user bindings, shared by builtin and Feature operations.
Hello returns canonical capabilities; bind<Request, Reply> compares full operation and schema metadata once.
A BoundOperation holds the server's random 128-bit session and generation key. There is no business enum,
universal command packet or builtin dispatch switch. Operations.hpp defines only actual builtin data consumers.
Handlers and descriptor metadata remain immutable and loaded through shutdown. External handlers execute on the
server owner thread and must settle synchronously; Runtime-owned Readback/compile continuations are bounded and
settled before the Responder. Merely returning success does not publish a successful business terminal.

Create/register occur before run. The host stops and joins run before destroying the server. Native
InstanceContext outlives DeviceSession. No CPU background workers are created by this implementation.
All received request borrows are settled before Responder reset, on successful shutdown and partial
initialization failure. Undeliverable input owners are retained in a bounded cold-disposal list until
shutdown, rather than releasing large assets in the frame loop. Main and Upload are the only two
long-lived clients admitted per epoch; per-request requester churn is outside this integration contract.

Owned requests carry one retained-byte lease in their IPC input owner. It reserves encoded wire bytes,
all complete backing capacities (duplicates charged conservatively), fixed envelope and maximum reply bytes.
Short requests with replies bounded to 128 bytes use SBO; their small reservation moves from pending metadata
to Completion without an allocated envelope. The shared budget outlives any retained completion. A reply keeps the returned input and lease until its result owner is released. Abandon removes
interest, not executor ownership or budget; tryReceive consumes returned abandoned input in bounded batches. Failed
submission leaves caller payload values and asset pins intact. Caller-owned references outside
the IPC use are not part of this retained-use budget. Ordinary metadata/codec allocations are not
represented as zero-allocation communication.

IPC attachments are borrowed from one exclusive Input owner, not independently reference-counted by
IPC. G2 adaptation pins the actual ReceivedRequest. Config metadata is compared in full before G5
assembly. Attachment Payload bytes are never copied; the reference and all synchronous G5 callbacks
finish before completion transfers the input owner. Input, pipeline and GPU completion lifetimes are
separate. G8 StateCommitted does not claim GPU readiness. Unsupported operations fail explicitly.

Each Effect owns its only editable association list. An ExecutionGroup has exactly SceneId, ViewId,
TargetId and ExecutionContext*. Scene owns View; View contains its color state, not Effect/Target IDs.
Attach prepares a complete candidate and publishes the group only on success. DeviceSession owns the
stable contexts, which pin shared immutable code and lease its scoped buffers. The single target writer
rule is checked on attach. Compatible resize replaces backing/context and advances target_epoch without
changing TargetId generation or compiling code. Unsupported format/sample changes preserve last-good.

The frame loop round-robins effects/groups with pre-resolved context pointers. No Feature callbacks,
analysis, compilation, name lookup or identity lookup occurs in stable record/execute. Incoming commands
have a bounded per-tick budget. The current host loop sleeps 1 ms between ticks; it is not a latency-tuned
product scheduler. Client codec/command allocations and complete API latency are measured separately
from the allocation-free stable rendering window.

Readback holds its actual source ReceivedRequest and pauses context reuse until the real queue ticket
completes and CPU-owned bytes have been produced. Cancel/Detach releases that logical reader before
ticket-driven context retirement. Stop settles all requests before Responder reset, then drains Native
work before destroying backing, device and instance. There are no borrowed-input worker tasks to join;
the host must join the run thread. Failed Native initialization follows the same IPC cleanup order.

This is the G9 minimal offscreen path: RGBA8, one layer/mip/sample, graphics push-color and transfer.
It is not WSI, dynamic History, professional asset storage, arbitrary GPU-ready Feature work or ECS integration.

Completions are move-only values owning IpcReply directly. tryReceive requires inactive output and moves one terminal event to the caller; no per-request result cache or shared Reply wrapper exists.

G2's BorrowedWireView and encodeWireInto are synchronous byte contracts, not additional ownership. The common
validator serves both owned and borrowed bytes. validateBoundWire is used only after immutable metadata has
passed cold registration/binding; it still validates values, complete ranges, attachment indices and limits.
RenderRequest::decode<T> has the handler precondition that T is its registered payload type. No fabricated
SharedBytes owner is used for SBO decoding. Large Feature assembly alone pins its real moved request for G2 views.

The test-only ControlFixture adapts old behavioral scripts to typed operations and is never installed. Its old
CreateScene timing now includes test-side cold binding and is not the new API benchmark. FeatureOperations.cpp
binds once and measures actual encoding/submission, wait, successful consumption/decoding and handler/reply
separately. The 1 ms loop idle remains visible in end-to-end results; zero engine new calls does not measure
driver/internal OS allocations. Failed publication retains any owned output until cold request disposal.
