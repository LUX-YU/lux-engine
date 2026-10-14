# Platform IPC R2 contract

R2 supersedes the API and per-message retention contract in historical IPC-D1
notes; those reports remain evidence for their original commits. Design source:
`LuxEngine_Platform_IPC_Refactor_R2_2026-10-14.md`. Baseline:
`d29b6d448b9f7527431ead5fb8765457ce28875d`. Only IPC and its consumers change.

## R2-0 ownership and linearization

```text
Core -> stable Name -> current/retired Channel -> owned Link list -> two SPSC Rings
                            ^                        ^
                      role participation       borrowed by Requester,
                                               Responder round-robin view,
                                               ReceivedRequest
ReceivedRequest -> unique input bytes (must settle before Responder reset)
IpcReply -> unique result + original input (may outlive all roles)
```

Only role acquisition/release modifies the Channel lifetime count. Each Channel
owns every published Link until its final role exits and name readers are safe.
The Link ownership list and pending/round-robin view use separate next fields.
Only Channel destruction traverses the ownership list and deletes Links/Rings.
ReceivedRequest never retains Channel/Link storage; its source borrow must be
settled or destroyed before its Responder resets. Responder operations and request
settlement are serialized by their logical owner, including across thread handoff.
Requester operations/reset are likewise serialized for that requester only.

| Interleaving | Linearization / storage safety |
|---|---|
| Concurrent same-name create | Existing sorted immutable-anchor CAS protocol prevents duplicate anchors. Positive role-count acquisition occurs inside the name reader grace period; zero never revives. |
| Final role release vs name discovery | Detach current, publish retired batch, collect only after readers leave. Preserve batch-exchange-before-reader-check and republication/recheck; final reader or collector completes reclamation without a later IPC call. |
| Link creation vs Responder close | Creator already owns a role. Publish unique ownership before the pending view. Closing the pending list rejects late adoption; the owned Link is still destroyed exactly once with Channel. |
| Requester exits during Responder polling/reply | Store Link closed then relinquish role. Responder's role keeps all Links/Rings stable. A concurrent already-started pop/reply may finish safely; later calls skip the closed Link. |
| Responder exits during Requester send/pop | First clear all borrowed requests, then store CLOSED and detach the epoch. Remaining Requester roles retain Rings. A send racing close may be accepted with unknown business outcome. Published replies are drained before CLOSED is returned. |
| Last role exits with unread messages | No role can still access any Ring and no ReceivedRequest may borrow it. Cold final Channel destruction discards unread messages directly, without generating terminal receipts or waiting for consumption. |

There is no per-message atomic reference, Ring access CAS, wait/notify, worker or
hot garbage collector. A responder-only plain outstanding-borrow count may enforce
the shutdown precondition; it is not an owner or a cross-thread reference count.
An arbitrary owner deleter is cold work: disconnected request disposal and final
role destruction must run outside a latency-critical Tick.

## API and memory contracts

`tryReceive(out)` returns RECEIVED/EMPTY/CLOSED; only RECEIVED changes output.
An active Responder requires empty/settled output on entry, even when no Link is ready.
This precondition is checked once before scanning. Reusing a still-owning
reply output can release a large owner and violates the caller's hot-path contract.
Send/reply failure preserves input/result. Reply credits return only on consumption
of a normal reply or an ABORTED receipt while the service is operating. Closing
does not synthesize business cancellation, GPU completion or success.

Message inline ABI is fixed at 128 bytes. Inline moves copy only initialized bytes;
owned moves transfer metadata only. Owner spans are caller-proven stable borrows,
not validated ownership merely because a pointer is non-null. Deleter modules must
remain loaded until their owners are released. No cross-process ABI is promised.

Known space tradeoff: a long-lived Channel retains closed Requester Links and any
unread payloads until its last role exits. Repeated short-lived Requester churn
therefore grows that epoch's retained storage. Stable Name anchors remain until
Core destruction and lookup is linear. The intended consumers are a small number
of long-lived Main/Upload roles; cold Link compaction is deliberately not built.

## Frozen Render host lifecycle contract (R2 closeout)

The future Render integration uses at most **two cumulatively created Requesters
per Server epoch**: one Main and one optional Upload. They live for the epoch,
not for a task, asset, frame or temporary worker. Moving a role under external
synchronization does not create a Link. Startup failure ends the entire epoch;
retry must not allocate replacement Requesters against the still-live old epoch.
High-frequency creation/reconnection is outside this integration contract. IPC's
generic factory does not enforce this Render-specific limit. A future consumer
requiring churn must first implement and qualify cold Link reclamation; it cannot
claim bounded retention from a bound on simultaneously live Requesters alone.

At default depths, two Links allow at most 128 accepted requests whose replies
have not been consumed. Queued inputs, borrowed requests, and queued replies share
these credits; they are not three independent pools of 128. On this Windows x64
ABI, the Ring element arrays total `2 * 64 * (160 + 328) = 62,464` bytes, excluding
Channel/Link/queue metadata and external allocations. Closed Links and their
unread owners remain included until the final role leaves that epoch.

**There is no finite payload-byte bound in IPC.** An arbitrarily small span can
retain an arbitrarily large backing allocation through its holder. G8 must supply
finite Main/Upload retained-owner budgets, B_main and B_upload, before admitting
asset work. Count full retained backing, not just the visible span; retain the
budget lease until the final owner release, including a reply held after its
credit was returned. With that host contract, retained payload is bounded by
B_main + B_upload. Without it, the byte upper bound is unbounded; 128 credits
alone are not a memory budget. No numeric budget, budget implementation or G8
qualification is asserted here. IPC remains independent of asset/GPU policy.

The future host must use this teardown order on normal exit, initialization
failure and cancellation:

1. Stop new submissions and asynchronous work admission; request task cancellation.
2. Join workers or otherwise prove they have stopped borrowing input. Workers may
   borrow bytes while the owner retains ReceivedRequest; they must not concurrently
   dispose it or publish replies through the responder.
3. On the responder's logical owner, complete or reset every ReceivedRequest and
   clear deferred work. Transport ABORTED is not business cancellation. Cancellation
   request alone does not establish completion or permit destroying its input.
4. Reset Responder only after the outstanding borrow count reaches zero. No callback
   may subsequently use it. Initialization unwinding follows the same order even
   if only part of the work collection was constructed.
5. Release remaining Requesters outside the latency-critical Tick. Final-role cold
   reclamation releases orphaned queued owners. Already-consumed replies own their
   bytes independently and can outlive all roles. GPU retirement is a separate
   future Native requirement, not proved by this CPU sequence.

Explicit stop/join/settle is required; member declaration order alone is not a
cancellation protocol. If using reverse member destruction as a fallback, declare
Responder before deferred work and worker owners, so the latter are destroyed first.
The `closeout` test covers these host recipes; there is no production RenderServer
in this stage, so G8 must validate its actual shutdown paths when implemented.

Requester close overlapping reply publication has two valid outcomes. If the
publisher observes closed, it returns CLOSED with input and result unchanged. If
it already observed open, publication may succeed after requester close. The
Responder's participation retains both Rings and the unread owners until cold
final teardown. Successful publication does not promise that a closed peer will
consume it. Controlled tests pause immediately before and after Ring publication,
then close the Requester and verify exact input/result destruction counts.

## Test migration

Retain directory/grace, source isolation, credit/rollback, FIFO, A/B progress,
Protocol, DLL owner and installed consumer tests. Replace only superseded behavior:
deferred work outliving Responder is now prohibited; requester reset no longer
waits for an active Ring access or frees its Ring early. The replacement controlled
interleaving checks reset completes while a Responder is paused with a stable Link.
Add unread queue destruction at final role, output non-mutation, fixed ABI, and
effective-byte move accounting. No historical qualification is reclassified.
