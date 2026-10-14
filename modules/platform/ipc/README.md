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
Output must be empty/settled before a successful receive. Reusing a still-owning
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

## Test migration

Retain directory/grace, source isolation, credit/rollback, FIFO, A/B progress,
Protocol, DLL owner and installed consumer tests. Replace only superseded behavior:
deferred work outliving Responder is now prohibited; requester reset no longer
waits for an active Ring access or frees its Ring early. The replacement controlled
interleaving checks reset completes while a Responder is paused with a stable Link.
Add unread queue destruction at final role, output non-mutation, fixed ABI, and
effective-byte move accounting. No historical qualification is reclassified.
