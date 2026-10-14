# G3-R-IPC lifecycle contract and proof obligations

Baseline `05e4d385d59b05a02baa017bc05feb1e3543508b`. Core/Data/Protocol, historical
reports, 719 Legacy blobs and the existing settings modification are read-only.
Authority: IPC-D1 / Draft7 / Plan v4, 2026-10-13. This document specifies the
implementation; runtime qualification is reported separately at exact I.

## Physical ownership and cold operations

One shared platform_ipc binary exports one Core. Core owns stable name anchors,
Channels and Links; roles and deferred requests own use rights, never one another.
Name anchors remain until Core shutdown. Channel and Ring storage does not.

Role create/reset/destruction is cold. Final disposal of disconnected deferred
work is cold too (IPC-D1 section 3.3). A closed tryReply preserves both received
input and caller output; callers move them to their cold disposal point. No hidden
worker or future collect call is required to dispose the last use. Calling an
arbitrary user deleter from a hot completion callback would violate this contract;
an epoch counter alone cannot provide a cold executor.

Requester reset closes access to its Rings and waits only for existing short Ring
access sections. Those sections never call user code, run codecs, wait for business
work or hold a deferred request. The waiter runs on the cold reset caller; the final
access exit notifies it. It then destroys queued owners and both Rings on that cold
caller. Deferred input has already moved out and stays alive independently. This
is a cold grace-period wait, not a lock in request/reply, and it does not wait for
deferred completion. Other Requesters continue to make progress. Host scheduling
can delay cold reset; reset is explicitly not wait-free.

Responder teardown invalidates its Channel epoch, detaches it from the active name,
closes pending publication and relinquishes its Link views. Published replies stay
in requester-owned-use Rings until consumed or requester cold reset. Late work pins
only stable metadata plus its own input; it does not keep closed Rings alive.

## Linearization and memory safety

Cold directory atomics initially use sequential consistency. Rings retain lux-cxx
acquire/release publication. Pointer/count atomics must be always lock-free.

| Operation | Linearization and ownership |
|---|---|
| Name insert | CAS into sorted append-only immutable anchors. Re-search on failure prevents duplicate names. |
| Channel acquire | Increment anchor readers before pointer load; CAS positive Channel use count before reader exit. Zero cannot revive. |
| Channel create | Publish fully initialized candidate with initial use by CAS from null. Losing creator deletes its private candidate. |
| Epoch acquire | Channel state WAITING -> OPEN CAS. One winner; OPEN rejects another responder. CLOSED is never reused. |
| Epoch close | OPEN -> CLOSED; CAS active pointer away, close pending publication. New same-name roles use a different Channel. Old pinned Channel addresses cannot be reused. |
| Last Channel use | Zero count; detach from active anchor, push to retired list, attempt collection. |
| Reader exit | Last reader attempts collection. Reclaimer detaches retired batch BEFORE checking readers. If readers exist, republish batch then recheck; this prevents missing the last reader exit. |
| Link publish | Release CAS pending list; producer never dereferences old head. Exchange transfers pending pins to sole responder. Closed sentinel rejects late publication. |
| Ring borrow | CAS increment access word only when closed bit is absent. Reset sets closed bit before waiting for the count to reach zero. No new borrower can enter. |
| Ring destruction | Requester reset is the sole destroyer after closed+zero. Requester operations are externally serialized with its own reset. |
| Link destruction | Requester pin, pending/view pin and deferred pins all gone. Hot final view release can delete only empty, fixed-size metadata: requester already destroyed Rings before relinquishing its pin. |
| Deferred disposal | Successful reply moves input to reply record before dropping use. Closed disposal is cold and retains input until that point. |

Channel reclamation proof: a detached batch contains only objects removed from the
active pointer before batch publication. If the subsequent SC reader-count load is
zero, any later reader must observe an active pointer that cannot reference this
batch. Earlier readers have either left or acquired a positive use (which excludes
retirement). If nonzero, republishing then rechecking closes the last-reader missed
wakeup window. Multiple collectors own disjoint exchanged batches. Names never
reclaim during operation, preventing directory-node ABA.

Pins do not form a cycle: Channel owns pending/view membership, but those internal
memberships do not increment Channel use. Public roles and ReceivedRequest do.
Last Channel destruction drains pending pins; no responder view can exist without
a responder role pin. A deferred pin can outlive both roles, but its final release
is cold and can directly complete Channel reclamation.

Closing a requester cannot wait for its own future deferred completion: only the
bounded Ring access scope is counted, and that scope ends before returning a
ReceivedRequest. Both close-before-publish and publish-before-close are safe:
failed publication leaves caller input; accepted publication is fully visible
before the borrow exits and cold reset drains it afterwards.

## Behavioral migration

Reuse lux-cxx SPSC unchanged. Replace the old Listener lifetime precondition,
shared Channel backing, thread IDs, 16 MiB byte limit and use_count encoding pools.
Preserve source-local replies, one credit per outstanding reply, FIFO dispatch,
deferred ownership, A/B independent progress and pure Render protocol contracts.
Old Connection deletion is gated on equivalent new tests. No production renderer,
Graph, Native, Engine scheduler, new business ID allocator or IPC worker is added.
