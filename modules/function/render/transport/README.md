# Render Transport (R2)

`render_transport` is a C++20 static component. Its only component dependencies
are `render_core` and the generic `lux-cxx::concurrent` headers. It owns communication
storage, admission and wake mechanisms; it creates no thread, renderer, Runtime,
Feature, graph, ECS integration or domain schema. Core and frozen V1 are read-only.

## Public surface and assembly

| Header | Contract |
| --- | --- |
| `Error.hpp` | Twelve predeclared structured protocol errors and descriptor span |
| `Operation.hpp` | Lane/kind traits, owned value constraints, `BlobRef`, `ExternalDataRef`, descriptors |
| `Routes.hpp` | Cold registration, compatibility validation, typed binding, owner-scoped generation |
| `Replies.hpp` | Typed numeric tickets, polling inbox, explicit deferred promise, epoch wake |
| `Packet.hpp` | Move-only PROGRAM/CONTROL/UPLOAD packets, typed encoding, borrowed dispatch view |
| `Transport.hpp` | Bounded admission, typed submit/poll entry points, stop and drain observation |

Create `RenderRouteTable(capacity)` on the intended PROGRAM/CONTROL producer
thread, register operation types and retain their `BoundRenderRoute<T>`. Move
the table into `RenderTransport::create`. The table is then immutable. `bind<T>`
is a cold owner-thread operation; it resolves stable identity and checks name,
wire/layout version, native layout, lane, kind and reply compatibility once.
There is no live registration/removal protocol in R2. A later real installation
safe point must define that protocol before mutable Feature installation.

Local route `{index, generation}` is **not** an identity outside its table.
Every bound route, packet and reply token additionally carries the owning
transport scope. An identical slot/generation from a second instance is rejected
before access. Removal accepts a bound route rather than a naked local ID.
Moving a table transfers its scope and invalidates the source. Slot reuse advances
generation; exhausted generations retire. Owner/thread identities are monotonic
within this linked Transport component, never serialized or accepted across
processes. Independently embedding duplicate static components in separate DLLs
is not a qualified interoperability configuration; installed/plugin linkage is
deferred to R16/R17. No global handle registry or Core handle change is introduced.

## Threads, admission and ownership

PROGRAM and CONTROL each reuse `lux::cxx::BoundedSpscFrameRing<PacketStorage, 4>`.
Only the assembly thread writes/submits these packets. One consumer thread binds
on the first PROGRAM/CONTROL poll and owns both lanes; wrong-thread calls return
an error before touching SPSC state. Reentrant SPSC dispatch is rejected. The
configurable pending bound is 1..3 packets per lane. All ring slots are reserved
before publication. Submit swaps storage; rejection restores the exact candidate,
including its reply reservations and attachments. The caller retries that same
candidate or explicitly abandons it; no silent replacement/drop occurs.

UPLOAD supports multiple producers and consumers. Each producer owns its packet;
each concurrent consumer owns a distinct empty scratch packet. The bounded ring
uses a short `try_lock` section and explicit BUSY/packet-count/byte-budget errors.
It is not lock-free. No dispatch callback runs under the mutex. Queued and active
dispatch bytes remain charged until callback completion. The byte charge includes
copied payload and retained attachment backing; explicit pins retained after the
callback belong to the consumer and are outside the queue's byte budget.

`make*Packet()` is cold allocation. Reuse returned empty packet storage after a
successful submit. `write`, `writeBulk`, `writeBlob` and `request` enforce lane,
kind and C++ payload type at compile time. Accepted payload is copied into aligned
owned storage. Blob offsets are packet-relative; attachment references identify
owned immutable byte vectors, never a borrowed span hidden behind a void owner.
`PinnedRenderBytes::copy` snapshots bytes; shared-vector construction requires
the caller to keep all aliases immutable. Dispatch views/spans borrow until the
callback returns. `retain` explicitly extends backing ownership.

Generated field checks reject pointers/spans/string views, even if trivially
copyable. Public values must be standard-layout, trivially copyable and at most
64-byte aligned. This is an in-process native-layout protocol, not a portable
network/disk serialization format. No extra field reflection occurs at dispatch.

## Replies, stop and cross-lane causality

The current implementation reserves a bounded completion cell before appending a
request. Replies are owned values up to 256 bytes. Capacity failure leaves the
candidate intact. Unconsumed results retain their reservation; the producer must
poll each ticket to a terminal result (including cancellation), or explicitly
`abandon(ticket)` through its inbox. Copies of a numeric ticket alias the same
single reception obligation; they do not reserve additional capacity. Losing all
copies without fulfilling that obligation is a caller lifetime bug and capacity
leak, not normal backpressure. An inbox can outlive the transport.
Inline completion has no per-request allocation or shared-owner increment.
`deferReply<T>` explicitly moves responsibility into a move-only owning promise;
abandonment settles cancellation. A generation CAS arbitrates completion,
cancellation and stop; late completion cannot overwrite a reused cell.

**Approved R2-FIX decision (2026-10-10):** chapter 03 section 13 now explicitly
permits pre-reserved cells instead of V1 pending reply publication/retry. The
historical R2 PARTIAL report remains unchanged.

`abandon(ticket)` ends reception, not the execution of an already accepted operation.
Later domain owners remain responsible for execution side effects. Pending or
terminal cells recycle immediately; WRITING cells become ABANDONED_WRITING and
the sole writer recycles only after its byte access ends. Therefore successful
abandon can precede capacity availability while a writer is active; it never waits.
Recycling advances generation (or retires an exhausted slot). Late completion,
packet cleanup or deferred-promise destruction cannot touch a new reservation.
Packet/promise cancellation terminates the operation with a readable error; it
does not discharge the receiver's consume-or-abandon obligation.

The receiver should serialize poll/abandon for one request. Foreign owner returns
`kTransportWrongOwner`; stale, already consumed or already abandoned tickets return
`kTransportReply`. Concurrent acquisition by a reader returns `kTransportBusy`
without relinquishing reception. A not-ready poll retains the obligation. The
abandon implementation follows at most three strong CAS attempts across the
monotonic PENDING -> WRITING -> terminal path; it never spins waiting for progress.
Normal completion adds one publication CAS, with no new per-request owner or heap.
Inbox/promise owners keep the arena alive; numeric tickets alone do not own it.

`requestStop()` closes the generic admission gate and settles pending replies.
Already admitted writers may finish publication. Existing callbacks may finish;
later drained packets are cancelled without executing callbacks. The host stops
new callers, joins them, and drains or destroys the transport. `drained()` is an
observation, not a thread join. A host may snapshot the wake epoch **before**
checking predicates, then wait on that snapshot; every progress/stop transition
notifies. There is no hot-path blocking wait or internal spinning fallback.

Lanes have independent order. An earlier UPLOAD or CONTROL submission does not
make a later PROGRAM safe. Readiness is an explicit reply, followed by publication
of the next PROGRAM revision. Ownership of accepted bytes is independent of
producer storage. This permits later readiness and retirement protocols but does
not implement GPU/resource retirement in R2.

## Algorithm provenance and bounded changes

V1 source is frozen at `a669409a289a6fa4092f21176397795b1cdb7f3e`; paths below
are relative to `render_legacy/modules/function/render/` and are never build inputs.

| Mechanism | Source / adaptation |
| --- | --- |
| Persistent SPSC, admission close/tickets | Reuse installed lux-cxx `BoundedSpscFrameRing.hpp` and `AdmissionGate.hpp`, source `bf779515a120350c7c5412c366eea73afdf59ccf` |
| Aligned packet/blob storage, retained capacity | `client/include/lux/engine/function/render/client/protocol/RenderCommTypes.hpp`, blob `d5ca1d49dc44ea67b1f188ca67e20d04fbf868b1`; typed boundaries and explicit capacity preflight replace old envelopes |
| Bounded upload ring and byte accounting | `runtime/pinclude/lux/engine/render/detail/UploadQueue.hpp`, blob `f832136e8402d881bac109e80dd5e5ef6a87a6bd`; try-lock returns BUSY, pop under lock permits concurrent consumers, active dispatch retains charge |
| luxop code generation | `cmake/template/comm_ops_hpp.template`, blob `90f098be06694c72d2f9952ffcc41a49b9df73b2`; same parser/multi-projection generator, new projection emits typed traits and recursive ownership checks |
| Stop/wake and retained failed submission | Generic admission ticket + epoch notification and unchanged-candidate retry; no old client/session/server hierarchy |
| Reply routing/publication | User-approved owner/generation pre-reserved cells; explicit consume/abandon and writer-owned deferred reclamation, not verbatim V1 migration |

`cmake/RenderTransportCodegen.cmake` exposes `render_transport_operations(target,
author_header)`. It uses the existing installed `lux_meta_generator` and
`lux_add_codegen_job`/`lux_codegen_add_projection`/`lux_target_add_codegen`.
The test author header uses existing generic annotation macros. Generated
registration metadata plus constrained packet/view templates provide encoding
and decoding; no second generator/parser or per-message lane/kind switch exists.

## Validation and limits

The bootstrap builds all six public headers independently, preserves Core's eight
tests, and adds nineteen Transport tests including actual compiler negatives.
CPU tests cover equal local slots in two owners, stale routes, ownership after
caller destruction, bounded retries, reply capacity, cancellation, deferred
completion, SPSC thread violations, 4-producer/3-consumer exact-once upload,
active byte retirement, stop races and deliberately reordered lanes. R2-FIX adds
10,000 single-slot cancel/abandon cycles, 1,000 concurrent completion/cancellation/
stop/abandon trials, late-generation and post-Transport checks. Two deterministic
tests pause the real completion/cancellation code after it claims WRITING, proving
that abandon cannot recycle storage early. That internal test-only hook is absent
from the production archive and unmodified benchmark target.

`render_transport_benchmark` runs ten one-million-operation cases. Timing is
p50/p95/max of 1,000-operation batch averages. Nine sequential cases warm up
10,000 operations; contention creates all workers/scratch packets before the
measurement barrier. Global C++ allocation hooks count new/new[]/aligned new
in the statically linked first-party code. They do not claim OS/CRT allocation
coverage. Cold construction, attachment creation and deferred promise creation
are outside the measured steady state. The V1 storage primitive is a mechanism
reference, not an equivalent full V1 transport comparison. No product, GPU,
frame-time or percentage speedup claim follows from these measurements.

Formal qualification must bind a clean implementation commit, run the tracked
snapshot validator, clone independently, configure, build full `all -j 4 -- -k 0`
twice, run CTest, benchmarks and `test/verify_r2.py`, and archive raw evidence
outside source. The auditor reads real File API and Ninja header dependencies;
compile negatives supplement these checks. Three-prefix header synchronization
is required but is not installed SDK or Android qualification. Generic error
value header installation closure remains deferred, as in R1.

`V2_PRODUCT = EXPECTED_UNAVAILABLE`. No R3 work is authorized by this component.
