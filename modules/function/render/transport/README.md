# Render Transport (Draft4 G3)

`lux::engine::render::render_transport` implements in-process, bounded routing and
terminal replies. It depends on Data/Core and lux-cxx container/concurrent/memory
facilities. There is no worker thread, RenderClient/Server, business dispatcher,
Feature installation, GPU synchronization or logging destination.

## Assembly and identities

Build a `RenderRouteTable` on one assembly thread. Registration copies the complete
operation name/version, Payload/Reply schemas, field names and enum domains. A
handler is a noexcept function plus borrowed context. All recoverable failures use
RenderResult; normal allocation failure is fatal. Register static error descriptors
at host assembly, not inside failure paths.

`registerOperation`, `remove` and `clear` are cold operations. Creation transfers
the table into a Transport and permanently freezes it. Handlers and contexts must
remain valid through stop and joined dispatches; deferred application work must
also retain any context it needs. Bulk handlers may run concurrently.

OperationId is the SlotMap's SlotKey. RequestId is the StableSlotMap's SlotKey.
Neither has a second generation counter. A RouteEntry caches its owner-assigned key
only for cold iteration/binding. The shared instance token distinguishes endpoints
even when a destroyed endpoint's address is reused. Old bindings/tickets retain
that token. Never persist local keys as process-global identities.

`bind<Payload, Reply>` compares full canonical names, exact versions and field
structures, including enum domains and dimensions. BoundOperation's template
arguments are its verified typed contract; its private key/token identify that
immutable route. Submitting bare OperationId is not a public API. No runtime hash
lookup, code generation or schema registry is involved.

## Sending and receiving

Obtain `TransportSender`, reusable `makeBatch()` and `makeRequests()` from the
receiver. Append preencoded G2 OwnedWireBytes and optional attachment owners, then
submit with a delivery lane. The output request collection must be empty. Failed
submit leaves the packet and all caller byte owners intact; successful submit
clears the packet and returns one move-only RenderRequest per record.

- COMMAND and PRIORITY_COMMAND each use a persistent four-slot SPSC frame ring
  with 1..3 configurable pending batches. The Transport creation thread is their
  producer; the first SPSC poll thread is their common consumer. Wrong-thread and
  recursive SPSC dispatch are rejected before touching ring-local state.
- BULK uses one record per submission and a bounded circular queue protected by a
  short try-lock. Multiple producer/consumer threads are supported. Contention
  returns Busy; full capacity returns Capacity. Retry belongs to the host.
- The host decides when to poll each lane. FIFO applies to command dispatch;
  batches are admitted atomically but execute non-atomically, continuing after a
  failed item. Deferred completion can reorder replies. Across lanes there is no
  ordering guarantee. Observe a prerequisite's successful terminal before sending
  an operation that depends on it.

Slot storage and attachment-vector capacities are prepared at construction. Before
publication every record reserves its Reply slot and required budget; any failure
rolls all reservations back. Physical queues carry only RequestId values. Complete
G2 Wire validation occurs before admission and again before handler invocation.
There is no native-layout memcpy packet ABI.

Command byte limits cover the complete batch. Bulk budget counts the wire bytes
and full visible range of every retained attachment, including unused or repeated
attachments, with checked addition. Attachment count, queue depth, Reply capacity,
command bytes, Bulk aggregate bytes and returned reply size are bounded separately.

## One Reply authority and distinct lifetimes

The shared TransportState uniquely owns the Reply StableSlotMap. Each live slot
has terminal/client/work state and a persistent auxiliary byte backing. Queues,
requests, tickets, Inbox and Promise all address that same state. Auxiliary
attachment capacity survives slot recycling; identity is never allocated twice.

The dispatched move-only ReplyPromise is the input-use lease. `input()` returns
a synchronous WireView borrowed from its stable slot. A handler must explicitly
call success, fail or cancel. For void replies, success still records a terminal.
Bad reply schema or bytes records Failed instead of leaving Pending. Destruction
without completion records Cancelled with `kTransportCancelled`.

To defer, move the **whole Promise** into the operation's work object. It retains
the input owner collection, Bulk BudgetReservation and shared communication state.
Keep it until the last controlled use of those inputs finishes, even if success
was already published. Copying a SharedBytes elsewhere does not transfer the
Transport budget lease; escaping asynchronous input use without its Promise is a
caller contract violation. WireView itself must never outlive this lease.

Client consumption or abandon releases the client claim. It does not release the
executor's input claim. A slot is recycled only after both claims end. Terminal
bytes returned by poll retain their byte owner and route metadata independently.
Duplicate completion/consumption, foreign tokens and stale generations fail.

Short reply critical sections protect identity and transitions. No handler, codec,
wait or arbitrary byte-owner destructor runs under a reply/queue lock. Input and
discarded output cleanup occurs outside locks before reuse. This is bounded
communication with locking, not a lock-free claim.

## Shutdown and waits

`requestStop()` closes Admission, cancels pending requests and wakes waiters.
Already-published terminals remain readable. A dispatch which already crossed
the stop check may still invoke its handler; its Promise protects its inputs.
Queued work is drained by polls or by quiescent receiver destruction.

`drained()` requires closed/empty Admission, empty queues, no active polls and no
remaining deferred input use. Unread terminal replies may still exist. Stop/join
active receiver calls before destroying the receiver. Deferred Promises, senders,
requests and Inbox may outlive it; they retain shared state, and senders return
EndpointStopped without borrowing the destroyed receiver.

Default consumption is nonblocking poll. To wait, snapshot wakeEpoch **before**
checking work, then call waitUntil with a deadline and optional stop token, and
recheck the condition. Waits exit for an epoch change, endpoint stop, caller stop
or deadline; a timeout is not a successful terminal. No fairness scheduler is
implied: the host must service every lane it enables.

StateCommitted, ResourceGpuReady and ResultAvailable are completion-contract
labels only. G3 cannot establish GPU completion, resource retirement or semantic
state commitment; only the eventual business handler can establish those facts.

## Verification boundary

Tests exercise typed routing, copied metadata, stale/foreign identities, exact
schema matching, bounded rollback/recovery, deferred/active input retention,
reentrant owner destructors, terminal/stop/abandon/consume races, SPSC FIFO and MPMC
exactly-once execution. Tiny-generation tests exhaust the same lux-cxx container
algorithms; production keys retain their full generation width.

The allocation probe counts global new/new[] (including aligned forms) after
warm-up, with preencoded inputs/replies and reusable batches/requests. Codec and
cold assembly allocations are measured separately or outside that interval.
ASan validates memory safety but is not evidence of data-race freedom.
