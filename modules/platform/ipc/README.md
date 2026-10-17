# Platform IPC

`IpcCore::instance()` is the shared-library entry point. A named service has one
Responder and multiple Requesters, each with independent, recyclable segmented
request/reply SPSC queues. Each role has one logical owner; the host serializes
its calls and reset, including thread handoff. IPC creates no worker threads.

## Objects and routes

`IpcMessage` uniquely owns one real C++ value. Its static TypeOps contains the
lux-cxx type hash, move operation and destructor. The 80-byte, align-16 SBO stores
eligible values directly; larger or over-aligned values use an external object.
There is no metadata header, business outcome, schema, per-message code pin or
runtime type registry. `get<T>()` requires the correct type (checked in Debug).
The host keeps all supplying modules loaded through their final destructor calls.

Request queue entries carry a generic 64-bit tag and correlation independently
of the message. `IpcRequester::trySend(tag, correlation, message)` transfers only
on success; failure preserves the value. `IpcResponder::tryReceive(request)`
returns the owned input, tag, correlation and an opaque `RequesterId`.

`RequesterId` borrows a stable name anchor, an unreused Channel epoch and a Link.
`trySendResponse(requester_id, correlation, message)` first compares the active
name/epoch, before dereferencing the Link. Thus an old route cannot address a
new service even if allocation addresses repeat. Requesters can independently
use the same correlation. Correlation uniqueness within a requester is the
application's responsibility; IPC does not allocate business RequestIds.

Reply publication does not consume the original ReceivedRequest. The receiver
releases input as soon as its actual consumers finish, retaining only the route
when needed. `tryReceive(correlation, output)` changes output only on RECEIVED;
output must be empty then. Published replies are drained before CLOSED is
reported, and a consumed result may outlive both roles. Close does not synthesize
business cancellation or GPU completion. A send racing closure can have an
unknown business outcome.

## Lifetime and concurrency

Core owns immutable sorted name anchors. A Channel uniquely owns every Link and
its two queues. Role participation and the name-reader grace period retain that
Channel; request and response values do not add Channel reference counts. Before
resetting its Responder, the host settles its pending operations and response
routing. Source-value lifetime is independent and must cover any borrowed data.

The existing creation/publication/reclamation linearization points are:

- Sorted name CAS prevents duplicate anchors. Channel use is acquired inside the
  reader grace period; a zero role count is never revived.
- A Link enters the unique ownership list before its pending receive list. The
  closing sentinel prevents later adoption; ownership still ends with Channel.
- Requester reset closes its Link before releasing participation. Responder
  participation keeps an overlapping receive/reply publication safe.
- Final role release detaches the Channel and publishes retirement. The final
  reader/collector rechecks after republication, so no later IPC call is needed.
- Final Channel destruction discards unread values after all queue users end.
  Arbitrary large-owner destruction is cold work, not a hot polling sweep.

Queue publication/recycling uses the existing SPSC acquire/release protocol.
Segment allocation failure preserves input; segments can grow and be reused,
without an artificial total-request count limit. A responder keeps its rotation
cursor, adopts at most one Link and visits at most eight Links per receive call.
The host bounds total work per iteration for fairness.

`workEpoch()` / `waitUntil()` provide a process-wide wake hint for host event loops.
Take the epoch before checking queues; successful request publication increments it and wakes waiters.
`notifyWork()` uses the same lifetime-safe Core for CPU completion and stop notifications, without registered callbacks.
A host must retain a bounded deadline when it needs GPU observation or has inspected only part of the bounded Link roster.
The hint does not replace SPSC publication, own messages, or imply that a particular channel has work.
