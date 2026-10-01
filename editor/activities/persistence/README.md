# Persistence activities

Existing domain activities, relocated with their separate real targets and ownership.
Author state remains in authoring; workbench and application are consumers, never dependencies.


## Frozen saves and ordered publication (P05)

The application owns one `WriteCoordinator`. SaveService and other migrated producers borrow it;
there is no per-window, per-model or per-producer coordinator. A producer resolves an address through
its store, reserves a ticket before encoding, and supplies owned bytes. Encoding order is independent
of publication order. An anonymous producer does not inherit another writer's file precondition.

`SaveService` owns stable operation records (`Impl::Operation` is the SaveOperation role), not Sessions.
`SessionStore` remains the sole Session owner. `SessionState` remains the sole checkpoint, binding,
observation and admission owner. Registration is an owner-thread RAII borrow: destroy/revoke its token
before the adapter. Pending records pin its code, never its Session. A revoked role cannot receive
baseline adoption, but its physical publication result remains queryable.

Owner-thread service calls have a private RAII dispatch scope spanning role callbacks and their
cleanup. Read-only status, registration-token destruction and completion of an admitted encoding remain
available in that scope. New requests, encoding dispatch, cancellation and acknowledgement return BUSY;
recursive adoption defers to the outer traversal or next
safe point. This is a service execution guard, not a second Session admission state. Describe and
capture recheck the original registration on return; revocation rejects that request and releases
its reserved capacity. A newly registered role cannot substitute for the revoked role. An already
executing accept may finish, but no subsequent call is made through a revoked registration.
Sources and the service must outlive callbacks already executing on their stack.

An ordinary save captures committed content on the owner and immediately releases READING admission.
`EncodeWork` carries only an owning `OwnedEncodeJob`; its code lease outlives the virtual destructor.
`SaveExecution` binds this work to existing ExecutionRuntime CPU/Blocking schedulers. Completion
collection settles encoding and disk facts; the host separately calls `adoptCompletions()` at a safe
point. A read in progress defers adoption. Do not hold ReadViews, Pane or Session pointers in jobs.

An ENCODING operation accepts its one owning completion on the owner even when collection first occurs
inside describe, capture or accept. The leaf only releases that operation's charged snapshot bytes and
supplies its existing coordinator ticket with encoded bytes or a typed failure/cancellation. It never
calls a source, adopts a baseline, destroys a rebind candidate or erases an operation. The job/code lease
was already moved into EncodeWork. Nested completion restores the previous dispatch protection, so the
outer callback still cannot recursively adopt or acknowledge. No second result queue or encoder retry
is needed. SaveExecution checks delivery instead of discarding its return; its once-only owner delivery
cannot legitimately be rejected. A wrong owner, stale ID or duplicate/non-ENCODING completion is a caller
contract violation, not temporary capacity pressure. Direct callers still receive structured errors.

Save As uses the same service/ticket path. Each domain supplies a private PreparedRebind, containing
an existing BindingChangePermit and preallocated binding/target values. It stays on the owner. After
publication, application moves those values and updates the existing checkpoint; it never reloads,
clears history or recreates a graph. The author node/object/pin/variable identity domain is unchanged.
Scene package envelope identities are rewritten by the shared `copyScenePackage`; author identities
are preserved. Unknown root identity references/indexed copy need an appropriate builder and are
rejected before IO. Ordinary Scene encoding retains unknown component payloads and package extensions.

PublicationUnknown retains the encoded artifact and isolates its lane. `reconcile()` requires both
writer retirement and a definite storage result before a successor runs. Unknown Save As releases
its permit; subsequent verification updates the disk fact only, never revives the abandoned rebind.
Export Copy always leaves the Session checkpoint and binding unchanged.

Capacity limits apply to active saves, retained terminal records, charged snapshot content, reserved
tickets and encoded bytes. They are logical retained-content bounds, not a claim to bound RSS, plugin
allocator internals or encoder temporary peaks. Ack removes only definite terminal observations.
The bounded record vectors use linear lookup (and bounded predecessor scans); no new task framework,
global cache or unbounded version list is introduced. Registration count follows live role ownership,
not the concurrent-save limit.

Each retained successful coordinator record proves a directed version edge: the effective
precondition supplied to the store and the confirmed output version. Within the same canonical
target, valid Session/Binding origin and uninterrupted chain, either endpoint proves continuity
for a later ticket. This permits an early receipt to be acknowledged while its published successors
still await adoption. Reserved, failed and Unknown records do not establish such edges. Publishing
still compares the controlled lane version against the actual file; it never refreshes that
precondition unconditionally. A different writer, or a successful write consuming a newly observed
external version, starts a new chain and excludes earlier edges.

The effective precondition stays in the existing PublicationQuery after successful completion;
there is no separate receipt archive or version list. Records remain bounded by WriteLimits::tickets,
are actually removed by acknowledge, and a lane is removed with its final record. Live records plus
one base/current version per lane are the only provenance storage. A completely drained lane needs
the producer's current observed version again. This does not widen the original single-coordinator
and optimistic external-version-check guarantees.

Shutdown order: stop submitting; drain/destroy SaveExecution while service/coordinator/store remain
alive; collect and reconcile physical responsibilities; revoke source registrations; release adapters
and Sessions. Destroying a service with an encode work item still outstanding violates its lifetime
contract. Closing a Session alone does not cancel or erase an accepted physical write.

The old `TAssetSave`, old tool IO/UI adapters and `SceneSaveCapture::copied` remain limited to the old
product until P12. New targets do not include or link them. Source, compiled-product and Workspace
producers borrow the same coordinator; no ordering is promised against legacy/external writers that bypass it.

P10Q keeps two STATIC components in this package: `editor_persistence` owns pure save/publication
coordination; `editor_persistence_execution` owns SaveExecution and its Process TaskScope binding.
The latter is optional for synchronous persistence consumers. Both source saves and derived products
borrow one WriteCoordinator. Frozen payloads use owning SharedBytes and count their full logical size
per accepted record, including Unknown quarantine. File IO belongs to storage::FileArtifactStore.

## File publication

`FileArtifactStore` publishes one file from owned encoded bytes. Existing ProjectStorage journal
code and FileArtifactStore share `editor_file_publication`'s file read/write/flush/replace primitives.
The kernel is a STATIC target in the activities/persistence directory, with a pure persistence dependency;
the backend never links the old ProjectStorage/Editor/editing implementation.

Targets are root-contained canonical filesystem addresses, including relative `.`/`..` aliases and
existing symbolic-link resolution. Windows keys fold case using the invariant locale. Known hard-link
aliases are rejected. Changing a symlink concurrently, case-sensitive Windows directories, filesystem
mount changes, and concurrent external writes between the final check and rename are outside the
single-coordinator guarantee. This is optimistic external-change detection, not global CAS.

The writer creates its own staging directory alongside the target, writes and flushes its payload,
rechecks the expected digest, then replaces the target. A pre-existing staging name is a failure,
never permission to truncate/delete someone else's file. Cleanup removes only this operation's
payload/directory. It is not a journal for project-wide multi-file transactions.

Windows uses FlushFileBuffers and MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH); POSIX uses fsync and
rename. A successful replacement is Published, separately from power-loss durability. Failure in the
optional post-replace durability callback produces a Published receipt with an UNCONFIRMED warning.
No directory-fsync or global crash-atomic guarantee is claimed. Reconciliation verifies bytes after
this synchronous backend has returned, which proves there is no detached writer left in the backend.

SaveExecution uses existing TaskScope RAII and CPU/Blocking senders. Neither its completion callback
nor a worker applies a Session baseline; SaveService does that separately on the owner. A foreign store
exception is Unknown, never guessed to be NotPublished.

