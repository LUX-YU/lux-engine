# Frozen saves and ordered publication (P05)

The application owns one `WriteCoordinator`. SaveService and other migrated producers borrow it;
there is no per-window, per-model or per-producer coordinator. A producer resolves an address through
its store, reserves a ticket before encoding, and supplies owned bytes. Encoding order is independent
of publication order. An anonymous producer does not inherit another writer's file precondition.

`SaveService` owns stable operation records (`Impl::Operation` is the SaveOperation role), not Sessions.
`SessionStore` remains the sole Session owner. `SessionState` remains the sole checkpoint, binding,
observation and admission owner. Registration is an owner-thread RAII borrow: destroy/revoke its token
before the adapter. Pending records pin its code, never its Session. A revoked role cannot receive
baseline adoption, but its physical publication result remains queryable.

An ordinary save captures committed content on the owner and immediately releases READING admission.
`EncodeWork` carries only an owning `OwnedEncodeJob`; its code lease outlives the virtual destructor.
`SaveExecution` binds this work to existing ExecutionRuntime CPU/Blocking schedulers. Completion
collection settles encoding and disk facts; the host separately calls `adoptCompletions()` at a safe
point. A read in progress defers adoption. Do not hold ReadViews, Pane or Session pointers in jobs.

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

Shutdown order: stop submitting; drain/destroy SaveExecution while service/coordinator/store remain
alive; collect and reconcile physical responsibilities; revoke source registrations; release adapters
and Sessions. Destroying a service with an encode work item still outstanding violates its lifetime
contract. Closing a Session alone does not cancel or erase an accepted physical write.

The old `TAssetSave`, old tool IO/UI adapters and `SceneSaveCapture::copied` remain limited to the old
product until P12. New targets do not include or link them. This stage does not migrate P07/P09/P11/P12
producers or promise ordering against legacy/external writers that bypass the shared coordinator.
