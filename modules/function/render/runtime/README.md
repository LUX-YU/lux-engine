# Minimal Render runtime

`render_client` has only Core/Data/Protocol and private Platform IPC dependencies. It does not include
or link Native. `render_runtime` supplies the actual Vulkan-backed server, assembled on a host-owned
thread. Its public header is neutral. There is no additional Endpoint, dispatcher or object registry.

The server owns Scene/Target/Effect SlotMaps. Each Scene owns its View SlotMap. An Effect owns its one
immutable compiled graph. Each client owns its connection-local RequestId SlotMap. The server owns
the frozen OperationId SlotMap; the initial Ready exchange compares complete canonical operation and
schema metadata. Random 128-bit session identities scope all resource commands to a server epoch.
Callers must carry the session returned alongside resource IDs; commands are never silently restamped.

Create/register occur before run. The host stops and joins run before destroying the server. Native
InstanceContext outlives DeviceSession. No CPU background workers are created by this implementation.
All received request borrows are settled before Responder reset, on successful shutdown and partial
initialization failure. Undeliverable input owners are retained in a bounded cold-disposal list until
shutdown, rather than releasing large assets in the frame loop. Main and Upload are the only two
long-lived clients admitted per epoch; per-request requester churn is outside this integration contract.

The one retained-byte lease travels inside the IPC input owner. It reserves the encoded wire bytes,
all complete backing capacities (duplicates charged conservatively), fixed envelope and maximum reply
bytes. A reply keeps the returned input and lease until its result owner is released. Abandon removes
interest, not executor ownership or budget; collectReplies reclaims returned abandoned input. Failed
submission leaves the caller's const command and payload pins intact. Caller-owned references outside
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
It is not WSI, dynamic History, arbitrary Feature handlers, professional asset storage or ECS integration.
