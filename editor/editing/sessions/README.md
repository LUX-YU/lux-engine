# Edit sessions (P01)

`SessionStore` exclusively owns published `IEditSession` objects. It is an owner-thread collection;
it does not read assets, save files, construct panes or run scenes. Concrete models are later phases.
The installed `lux-engine-edit-sessions` package can be consumed without the old Editor or Context.

Construction is `reserve<T>(kind, code) -> prepare(reservation, candidate) -> publish(reservation)`.
Reserve pins code and issues a domain/slot/generation identity. Prepare checks identity and type before
taking the candidate. Failure leaves ownership with the caller. A prepared candidate stays invisible;
abandoning its reservation destroys the candidate before releasing its code. Publish performs no
allocation or plugin construction. Reservations and permits must be destroyed before their owner.

`TSessionKey<T>` is issued by the store, never reconstructed from an asset identifier. `TSessionAccess<T>`
returns short owner-thread borrows; references must not cross reentry, waits or asynchronous callbacks.
Concrete session methods enforce their `EditGate`; access does not offer an unchecked erase operation.
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
belong to P05, not this module.

The slot declares `CodeLease` before its session owner. A concrete session must likewise declare its
source before history so retained edits are destroyed first. Native and installed tests exercise
history memento -> source -> code destruction, candidate abandonment, generation/type rejection,
checkpoint identity, permit lifetime and compile-negative scope copies/moves.

The old Scene/Material/FlowForge products temporarily use the private
`editor/transition/LegacyPersistenceState` compiled into `editor_editing`. Each working copy has one
checkpoint, moved with its history; pending requests prevent bridge movement. The bridge is not
installed and new modules cannot include it. It expires by P12. There is still only one history
algorithm, implemented in `editor/editing/history`.

P01-R1 separates the closing commit from presentation queries. The private
`IEditSession::currentContent() noexcept` reads only existing scalar identities, with no allocation,
IO, notifications, gate changes or publication. `SessionStore::close` calls it under CallbackScope
and still verifies the permit's content before consumption. Public `describe()` continues to return
owning presentation data and may allocate or throw; it is never part of the closing commit.
