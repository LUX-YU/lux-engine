# N2 Storage installation transaction prerequisite

N2/N3 remains IN_PROGRESS; this is not the final typed provider API. Factory admission now validates
its declaration, device capability and optional typed ConfigValue before the sole Registry type SlotMap
publishes an identity. It returns StorageTypeId. Configuration and factory/code pins have the same type
record lifetime. No instance or GPU allocation is created by this operation. Missing typed configuration
is rejected before invoking a factory even when no encoded DataSchema is declared.

The old configureType(), freeze()/frozen_ and Runtime storage_types duplicate registration collection
are deleted. Server installation completes immediately with the already-live DeviceSession, at the Owner
safe point; run() has no Factory/configure loop. All recoverable Command checks occur before the Registry
commit; after that point normal heap OOM is fatal and Command publication has no recoverable failure.
CommandMetadata no longer retains a Storage descriptor or availability flag; dispatch does not repeat a
capability decision already made at admission. Pending/Transfer/IPC/Queue/Graph algorithms are unchanged.

ListStorages reads the unique actual type table and returns owned names, IDs and required/optional bits
actually supported by this Session. It does not enumerate live instances or install a second catalog.
StorageTypeId/StorageId definitions move unchanged to existing core/Ids.hpp so pure Client includes do
not pull Resource/stdexec/Native. No aliases or forwarding headers remain at the former definition.

Added tests prove wrong Config and unsupported capability have no published identity or native creation;
late type insertion retains existing instances; missing Config cannot reach typed create; unavailable
optional capabilities are not advertised. Runtime performs real Owner-thread late installation, then
ListStorages and explicit Command refresh, with no partial Commands after failures. Duplicate rejection
preserves the old type and directory; returned owned names survive Server destruction. Existing Native
factory/rebinding/Texture/Buffer tests migrate to the single install call, retaining their assertions.
The old frozen-directory behavior is superseded by the authorized Owner-thread dynamic contract.

Implementation: complete all -j 4 -- -k 0, second no-work; focused 6/6 and full 106/106 passed (210.87 s).
Real GPU/SyncValidation and installed/relocated consumers are included. Route sizes: call16/align8,
entry24/align8, metadata160, request256 bytes. No equal-work performance conclusion; PARTIAL retained.
First working build failed on a missing direct Commands.hpp include in the new fixture; corrected and
all raw logs retained. Evidence: E:/RenderV2Rebuild/V12/evidence/N2/storage-install and corresponding
storage-install-*.console.log files. Legacy719, three user files, dependencies and IPC supplement passed.

Still pending: replacing public registration aggregates/native null-state binding with a single typed
Factory/owned Handler/member installation, idempotent provider reuse, final Hello bootstrap narrowing,
FeatureId/typed selection migration. N4 owner/lease/retirement work is not started; only removing frozen
is pulled forward as a necessary dynamic installation prerequisite. Independent V and final N7 remain
required. Root product historical BLOCKED; upper product and unrun platforms NOT_RUN. No push.
