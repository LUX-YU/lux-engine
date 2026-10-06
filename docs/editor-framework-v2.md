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

This table describes the target contracts, not completed implementation. Batch results belong to the working record.
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
