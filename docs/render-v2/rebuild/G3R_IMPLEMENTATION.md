# G3-R implementation checklist

Baseline: `411c5dcce06a7d259aa6783b71d3a14e63f96cec`, branch `codex/render-v2-rebuild`.
Draft6 SHA256: `c938b9e670049acaeebd3218f0a6197525caca4069051877711b6df316b88ba9`.

- Replace `transport/**` with installed `protocol/**` and non-installed `runtime/connection/**`.
  Protocol depends only on Data/Core; Connection privately uses lux-cxx concurrent/container.
  No business Client/Server, dispatcher, Graph, worker or GPU object is introduced.
- Protocol owns only value contracts. The fixture's one immutable Operation SlotMap supplies
  server-domain OperationIds. Listen takes an immutable, lifetime-pinned directory; schema metadata
  must have static storage, as in G2. Binding compares complete contracts once, and pins the server
  identity. Each Client owns its own Pending SlotMap and allocates RequestIds there.
- Every connection backing owns one request and one reply SPSC ring. Client owner writes requests,
  reads replies and exclusively mutates Pending; Server owner reads requests and writes replies.
  Endpoint handoff requires external synchronization. No mutex or atomic shared_ptr is needed.
- Listen/unlisten are externally serialized with connect. While the listener is stable, concurrent
  connect constructs independent owning nodes, then publishes them on a cold atomic pending list.
  Server exchange acquires that list and owns node reclamation. Closing before accept only marks
  that connection; shutdown first joins connect callers, then acquires and releases pending nodes.
- Each accepted request reserves one client-local Pending/Reply credit before release publication.
  Credit and byte charge return only on terminal consumption, including abandoned requests.
  Queue failure rolls back Pending without moving caller bytes. No central RequestCell exists.
- Deferred work owns origin backing, RequestId and bytes. Only the Server owner may complete it.
  Reply carries retired input owners to the Client; expensive ordinary input destruction therefore
  normally occurs on consumption, not the simulated render tick. Disconnected/teardown reclamation
  is an explicitly cold host responsibility after producers/consumers have joined.
- A closed connection reports OutcomeUnknown for unpublished results. Published terminals remain
  consumable. Server-domain fixture resources survive disconnect of their creating Client.
- Tests use a bounded rotating receiver (total and per-connection budgets), concurrent connect,
  colliding local IDs, deferred completion, isolated credits/bytes, paused A and progressing B,
  complete G2 codec allocation/latency measurement, ASan, and a relocated installed SDK consumer.
- Preserve G1/G2, 719 Legacy blobs, all historical reports and `.vscode/settings.json`.
  Root TOOLCHAIN missing later consumers is a separate BLOCKED product closure, not a G3-R pass.

## Implemented responsibilities and limits

`render_protocol` installs one header, `Protocol.hpp`, and a small validation implementation.
It contains Request/Reply headers, SlotKey aliases, stable Operation descriptors and completion
semantics. Its only public link is `render_data`, which already supplies Core and lux-cxx basics.
`render_connection` is an ordinary, non-exported CMake library with a project-only `sinclude`.
No Connection/Runtime headers or library enter the installed SDK.

`LocalListener` owns accepted Server endpoints. The test directory is a single immutable
`SlotMap<OperationDefinition, OperationTag>` whose IDs are allocated once by the fixture. Its
metadata is static, as required by G2; callers must not retain a mutable directory alias after
publication. This is an assembly seam, not Feature registration or a business dispatcher.
Bindings pin one server identity; connection backing pins the same identity and immutable directory.
Bare RequestIds are meaningful only in calls on their originating endpoint. ServerWork additionally
pins that origin, so equal numeric IDs on different connections never route through a shared lookup.
The server-global resource test uses a fixture token, not a production GPU resource implementation.
Bare resource SlotKeys still do not prove origin across server lifetimes.

Pending connection nodes own independent backing before release-CAS publication. Push never
dereferences the observed list head. Acquire-exchange transfers node ownership to the server;
budgeted accept keeps an unprocessed suffix server-local, so repeated arrivals cannot displace it.
A pre-accept Client close marks only its backing. Stop owns and releases both pending lists after
the host has joined Connect callers. Listen/unlisten are mutually serialized and may not race Connect;
the published Listener pointer is a borrow under that explicit lifetime contract, not a reclamation
algorithm. Simultaneous Connect calls during stable listening are supported and tested.

Only Client-owner code accesses `SlotMap<Pending, RequestTag>`; only Server-owner code accesses
active deferred-work counts and Reply producer state. A no-concurrent-use handoff requires
`adoptCurrentThread()`. Owner violations and impossible credited-ring overflow terminate in every
build configuration, rather than using an NDEBUG-disabled assert. Queue indices/flags and the
cold pending-list pointer are statically required to use lock-free atomics. Shared owners are
ordinary immutable shared_ptr pins, never atomic<shared_ptr>. No introduced mutex, condition
variable, spinlock, central RequestCell, event queue or global business MPSC exists.

Credit bounds are exact even when A never polls. Byte charges conservatively include whole visible
attachment spans, including duplicates, and remain charged until terminal consumption. Abandon
changes only interest. Client close can publish OutcomeUnknown locally; a later real reply is
drained without a duplicate terminal. Stop is server-owner ordered against completion: already
published replies remain readable, unpublished work remains pinned and yields unknown outcomes.
Publication racing Stop may be accepted without execution; acceptance never promises state commit.

`ServerWork` owns payload until completion, then puts retired input in the credited Reply. Normally
the Client performs the potentially expensive final destruction. A disconnected peer is reclaimed
only by explicit **cold** `collectDisconnected()` after its queue drains and deferred work ends.
This can run arbitrary byte-owner destructors and must not be called from the bounded hot receive
loop. Stop/teardown has the same cold responsibility. No hard real-time destructor bound is claimed.

Small encoding reuses independently owned byte blocks; retained output prevents a block from being
rewritten. No block owns its connection. Large byte payloads keep the existing SharedBytes owner and
pointer; only their attachment list is allocated/copied. Empty attachment lists have no vector object.
This avoids MSVC Debug iterator-proxy allocations without changing G2 codecs or byte ownership.
The current direct calls retain G2's full validation costs; no parallel parser was added.

## Migration and actual implementation checks

| Old G3 contract | G3-R disposition |
|---|---|
| PROGRAM/CONTROL SPSC + mutex Bulk | Replaced by one request/reply SPSC pair per connection using lux-cxx `LockFreeQueue.hpp` |
| Central StableSlotMap RequestCell and ReplyMutex | Removed; client-local SlotMap and pre-reserved terminal credits |
| AdmissionGate/BudgetGate/condvar | Removed from connection path; owner-local admission, conservative bytes, nonblocking poll |
| Shared ReplyPromise / deferred input | Move-only server-thread ServerWork retaining origin and input, one terminal writer |
| Owned Wire and full schema comparison | Preserved through unchanged G2; bind/codec tests verify actual operation contracts |
| One global endpoint / three lane scheduling | Same-address concurrent connections, bounded rotating fixture receiver |
| Shutdown, abandon, malformed reply, FIFO and source owners | Equivalent lifetime/negative tests in contracts, ownership, races and progress |
| Preencoded allocation experiment | Replaced by complete encode/send/receive/decode/encode/reply/decode measurement |
| Old structure-specific tests and installed Transport consumer | Removed with old source; original G3 evidence remains in Git/history and unchanged reports |

Implementation checks: Windows MSVC 19.44.35226, C++20 `/GR-`, CMake 3.31.10, Ninja,
lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`, toolset
`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`. The isolated dependency prefix contains
64 headers, all byte-matched to sibling lux-cxx. Debug and RelWithDebInfo all builds and
second no-work builds passed; CTest 20/20 in both, including all original 14 G1/G2 tests.
ASan all build and 20/20 tests passed; independent exact-I verification follows this commit.

Implementation measurements (not substituted for independent qualification): RelWithDebInfo
warm complete codec path 10,000 operations: 0 heap allocations/0 bytes, p50 0.5 us, p95 0.8 us,
p99 0.9 us. Cold two-connection assembly: 108 allocations/17,472 bytes; first operation 0 allocations,
16.5 us. Debug warm complete path also 0 allocations. These are same-thread round trips exercising
both physical rings and codecs, not thread-scheduling latency. During A's 1.2-second pause,
B completed 1,222,795 replies and the independent simulated CPU tick advanced 678,814 times.
Tick samples every 64 ticks: p50 1.8 us, p95 2.3 us, p99 2.7 us; full-run max 327.1 us.
The reverse flood completed 200 A requests. Raw queue/byte samples are saved with the tick data.

Two failures were fixed, with original logs retained: Windows sleep granularity invalidated an
early timing fixture, which now yields without imposing an artificial timer; Debug STL empty-vector
proxy allocations required the attachment-list representation change above. No acceptance threshold
was relaxed. All mandatory final qualification is rerun in the separate clone.

Public Protocol header synchronized into Debug, RelWithDebInfo and Android/lux-engine include
prefixes. Twelve obsolete G3 installed Transport headers were byte-checked against baseline and
backed up before exact-file removal. Original files, SHA256 inventory, logs, CSVs and protection
checks are under `E:/RenderV2Rebuild/G3R/evidence`; old evidence is untouched.

Root TOOLCHAIN reconfiguration remains BLOCKED by absent `render/client`,
`render/features/cmake/feature_client.cmake`, `render/standard_content` and their exported targets.
No placeholder or disabled consumer was introduced. GPU/Validation/SyncValidation: N/A;
Linux/Android build and TSan: NOT_RUN. No G4, Engine integration, push or merge was performed.
