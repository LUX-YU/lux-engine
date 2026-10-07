# Editor Framework v2

This migration replaces the Framework v1 Object/UI contract. It does not add authoring tools.
The working record is `.internal/editor-redesign/framework-v2/`; raw evidence is external to the repository.

## Responsibility and migration order

| Batch | Contract |
|---|---|
| V0 | Freeze legacy and verify historical evidence before moving it outside the source tree |
| V1 | Qualify the existing generational sparse container for bounded, prepared identity allocation |
| V2 | One DLL-provided ObjectRuntime; non-owning generational ObjectId registration |
| V3 | Private UI backend Context; Root retains object-aware orchestration |
| V4 | Non-owning parent links; Root uniquely owns all Panes and traverses content hierarchies |
| V5 | Detached Pane factories and canonical type/name/title descriptions; no EditorUIRoot owner |
| V6 | Immutable shared error descriptions and stable numeric errors, including active Scene failures |
| V7 | No RTTI in first-party active targets; clean build, SDK and rendering qualification |

Final implementation and verification scope are recorded in [the delivery record](editor-framework-v2-verification.md).
Batch history belongs to the working record; historical results are not substituted for final qualification.
Window, EngineContext and Root survive project switches. Panes die before project services. The existing SceneRuntime,
Process and Renderer remain the sole owners of their respective execution and retirement protocols.

## Reference source and evidence

`editor_legacy` is frozen reference source. The current build does not configure, install or link it. Reproducing its
results requires its historical Git revision. New foundation interfaces are not kept compatible for frozen source.

Before removing `dev_log` from the current tree, all 15,754 files (666,062,601 bytes) were read, hashed, archived and
verified by reading every archive entry. The archive is `historical-evidence-cf1bd7d64994.zip`, SHA256
`0852b321d37180aa43863f3147a53cd1cedf89cf8b41b3d641311b8c89bf2cff`.
Its manifest, original directory and separate user-patch backup are stored under the external delivery directory
`../archives/lux-engine/EditorFramework-v2/`. Existing Git commits remain unchanged.

The ProjectBuilder user patch remains unapplied. The pre-existing Pane.hpp comment-only change is also protected
separately. Historical PARTIAL/FAIL results, waived checks, native-input deferral, Linux/IME/sanitizer and historical
performance limitations are not converted into new passes by this migration.

## Qualification

The generational container prerequisite is lux-cxx `cf14ab1de6b2b96b56531a4de8bfa02efb4d7ca1`.
Clearing invalidates issued keys, exhausted generations leave reuse, and insertion preparation covers
dense, sparse and recycling storage without issuing identities. The engine uses the same container for ObjectId.

ObjectRuntime is supplied by the object DLL. The host establishes its thread before starting workers; independent
framework lifetimes reuse that Runtime. The old tests for multiple object thread domains are replaced with
wrong-thread access, worker delivery and singleton DLL identity checks. FULL, fixed batches, partial broadcasts,
revoked receivers and owner-thread reclamation retain their original assertions.

Use a clean tracked implementation commit for final qualification. Build `all -j 4 -- -k 0`, then confirm the second
build has no work. Run affected framework, Object/UI, services, Scene/Flow and render tests, PLAYER, fresh SDK consumers,
public-header checks and actual GPU regressions. Preserve failure/reentrancy assertions when updating test APIs.
Raw logs, command provenance and dependency SHAs stay in the external delivery directory. The repository retains
contracts and repeatable tests, not a second evidence archive.

## Dispatch and error boundaries

All active first-party targets compile without RTTI. `build.no_rtti` checks actual compiler commands and
compiles a positive case followed by rejected `dynamic_cast` and `typeid` cases. This does not change third-party
compiler settings or exception containment. Flow uses its narrow ScriptAbility capability, and the highlight backend
dispatches through the existing render-feature interface; operation tags alone never justify a downcast.

The error DLL owns process-wide immutable descriptions. Error values contain a stable name-derived ID and three
numeric arguments, never an owning or borrowed plugin payload. Framework boundaries and Scene build/execution causes
use these values. Local typed errors and complete domain diagnostics remain on their original owners. Render's local
registry is translated while its description is available; a temporary render slot is not a stable ErrorId.

Error descriptions are declared by their owning module and registered during assembly, before operations can fail.
Registration conflicts are assembly errors. A failure constructs only a stable ErrorId and numeric arguments;
it does not register descriptions, acquire the global registration lock or allocate diagnostic strings. Render
descriptions acquire their stable identity when admitted to the existing Render registry. They are copied into the
immutable error provider there, so formatting a previously produced Error remains valid after provider unload.

Ordinary heap exhaustion is fatal. `noexcept` semantic APIs do not promise to recover from `new`, STL allocation or
`bad_alloc`; their expected errors describe domain, capacity, validation and foreign-operation failures. No
recoverable allocation error may be advertised without a real producer. Explicit bounded capacity, GPU allocation
and foreign backend failures retain their distinct contracts. A callable/factory containment boundary must terminate
on `bad_alloc` before converting other foreign exceptions; hot paths do not acquire new exception handlers.

The installed consumer in `editor/tests/installed` uses installed public headers and libraries, including real Object
and Error DLL probes. The DLL probes check shared Runtime/Registry identity, destructor-tail code lifetime and error
formatting after plugin unload. Each listed public header is compiled independently under C++20 without RTTI.

## Final convergence before tool migration

The API convergence retains the v2 owners and execution algorithms. LuxObject has only detached construction;
parent links and UI addElement both support guarded reparenting. Elements use ObjectId for ImGui identity. PaneId
is private registration bookkeeping, not a caller-provided layout key. Root owns Panes but enumerates borrowed Pane&.
It retains one DockTree model and validates/prepares/commits internally. Menus own their cold labels and command IDs.

Root::update adopts queued changes itself. The maintenance overload cannot accidentally draw, and the drawing overload
pins resources synchronously. Root implementation is split by concern behind one Impl with grouped state, not separate
managers. Private EditorUiScene transport belongs to app; WindowInput belongs to UI implementation. The public Context
component registers factories, while createPane is supplied and declared by UI. Freeze and mutable factory lookup are
private. All error domains declare IDs before failure, with registration conflicts reported during assembly.

SceneToolRegistrar remains provisional until the first real SceneSession/SceneToolSet. No Scene/Material/Flow tool,
dynamic registration, new docking persistence or additional runtime is introduced by this convergence.

## Final lifetime/API contract

DockTree values retain opaque `PaneHandle` values, never Pane addresses. A handle identifies one
registration in one Root (Root ObjectId plus local generational PaneId); it does not keep either alive.
`paneHandle` captures that identity, and `resolvePane` permits only a synchronous owner-thread borrow.
Removal, remount, Root replacement and address reuse leave old handles stale. Layout application resolves
all handles before dereferencing any Pane and publishes nothing on failure. Runtime handles are not a
persistent layout format.

`PaneChanged` and `ObjectRemoved` are noncopyable synchronous borrows. Existing signal admission rejects
QUEUED connections as `PAYLOAD_NOT_QUEUEABLE`; no new signal dispatch or lifetime protocol is introduced.
UI-specific structural methods are public; generic LuxObject mutators are hidden on UI types and remain
rejected through explicit base access. Maintenance uses `Root::update()`; frame capture uses the explicit
frame overload. Installed headers do not grant test-only private access.

Assembly, UI/service/tool factories, frame capture and Pane enumeration encode `noexcept` in their callback
signatures. The original callback borrowing/owning split remains; semantic failures use expected, ordinary
heap exhaustion is fatal. Descriptors supply canonical names once; numeric IDs are derived from those
same declarations, and registration remains a cold assembly operation.

Assembly remains a synchronous `openProject` input in this correction; no stored lifetime contract is added.
PaneDescription.name is a unique layout input and factory parameter, not yet a framework-owned persistent
name binding. That binding belongs to a future Editor-side layout implementation, not generic Pane.
SceneToolRegistrar remains **provisional and excluded from API freeze** until a real SceneSession/SceneToolSet
vertical slice establishes its selection semantics. Vector registrars and their ownership are unchanged.

Noexcept callable support for this correction uses lux-cxx `0a0e7419fc7229df6e372cd35a540249f92250ef`.
