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
