# Editing

The shared `editor_contracts`, `edit_history` and `edit_sessions` targets own the pure identities,
history algorithm, SessionStore and SessionState. They share one physical public include root,
but retain their binary identity owners and separate installed packages.

ViewInfo is a pure observation; window errors and close preparation belong to the workbench composition provider.
EditorError is a pure value supplied by editor_contracts.

## History storage and execution (EC1)

`EditHistory` owns the sole log, state identity, cursor, limits, owner thread and phase.
`EditExecutor` is a stateless synchronous value in the same `edit_history` binary. It performs
execute/undo/redo/clear/close; the log no longer exposes mutation functions. Two executors operating
on one log share that log's gate, including preparation, publication and reclamation. The executor
does not own another cursor, queue or history and does not schedule background work.

The original algorithm retains its ordering: prepare without model mutation; apply and adopt history;
publish consistent facts; reclaim prepared/retired owners before releasing admission. A failed prepare
keeps the caller's operation, and NO_CHANGE leaves the redo branch and state identity unchanged.
Explicit close notifies once; log destruction silently releases retained operations under its original
owner-thread invariant. Models continue to own their log and source in the established destruction order.

## Edit sessions (P01, EC4 shared allocation)

`SessionStore` owns published logical identities and shares their actual `IEditSession` allocations.
It borrows the original Object dispatcher for final owner-thread reclamation. It is an owner-thread collection;
it does not read assets, save files, construct panes or run scenes. Concrete Scene, Material and Flow models live in authoring.
The installed `lux-engine-edit-sessions` package can be consumed without the old Editor or Context.

Construction is `reserve<T>(kind, code) -> prepare(reservation, candidate) -> publish(reservation)`.
Reserve pins code and issues a domain/slot/generation identity. Prepare checks identity and type before
taking the candidate. Failure leaves ownership with the caller. A prepared candidate stays invisible;
abandoning its reservation invalidates its gate and surrenders the allocation for safe-point reclamation.
The source and history are destroyed before releasing their code. Publish performs no
allocation or plugin construction. Reservations and permits must be destroyed before their owner.

`TSessionKey<T>` is issued by the store, never reconstructed from an asset identifier. `TSessionAccess<T>`
returns short owner-thread borrows; references must not cross reentry, waits or asynchronous callbacks.
Concrete session methods enforce their `EditGate`; access does not offer an unchecked erase operation.
`share(key)` returns a real shared owner of that same allocation, never an alias into an erasable Store slot.
Logical close invalidates the key immediately and commits `CLOSED` in the existing gate. A retained model
and its old read views reject editing/reading with `STALE_SESSION`; no second closed/current/dirty state exists.
The last shared release may occur on a worker; physical destruction remains at the original dispatcher's
safe point. The dispatcher outlives the Store and every surrendered allocation. Borrowed callback state
must also remain alive through that safe point; arbitrary lambda captures are not extended by the Store.
Store mutation is rejected during its plugin callbacks and reclamation. Closing requires a store-issued
permit tied to the same object and content stamp. Moved or consumed permits cannot close a reused slot.

`SessionState` owns the source binding, observation counter, persistence checkpoint and edit gate.
It borrows the current `StateId` from the one `EditHistory`; it does not copy the history cursor or
current state. All state operations are owner-thread operations. `withEdit` constructs a noncopyable,
nonmovable scope on the stack. A close or binding permit is move-only; abandonment releases admission.
Destroying a gate with a live scope/permit is a contract violation, not a recoverable shutdown state.

`ContentStamp` combines session and history content identity. Dirty compares current state and binding
against the optional `PersistedState`. Equal bytes and notification counts never establish clean state.
Opening a bound source can explicitly initialize a baseline (publication order zero); a new unnamed
source has none. Receipt adoption rejects stale history, binding and publication order. Duplicate exact
receipts are idempotent. Encoding, receipt validation against a storage backend and save orchestration
belong to activities/persistence, not this module.

The slot and host-side shared deleter keep `CodeLease` outside the session allocation. A concrete session must declare its
source before history so retained edits are destroyed first. Native and installed tests exercise
history memento -> source -> code destruction, candidate abandonment, generation/type rejection,
checkpoint identity, permit lifetime and compile-negative scope copies/moves.

P01-R1 separates the closing commit from presentation queries. The private
`IEditSession::currentContent() noexcept` reads only existing scalar identities, with no allocation,
IO, notifications, gate changes or publication. `SessionStore::close` calls it under CallbackScope
and still verifies the permit's content before consumption. Public `describe()` continues to return
owning presentation data and may allocate or throw; it is never part of the closing commit.
