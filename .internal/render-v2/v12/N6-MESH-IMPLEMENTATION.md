# N6.1 production Mesh Storage — implementation

Parent4f90e946f9e2e58c5558b7384f0253010a146577, the independent combined N5 V. Remote rechecked
78bcd220815f845466d85d31f994901e4db224ba. No push. N6 is still in progress; Material/Instance/Camera,
real Features and integrated example are not claimed complete.

Neutral MeshDescription/Attribute/Submesh describe one interleaved vertex stream and UInt16/32 triangle
indices, with exact byte/range/device format validation. MeshId is StorageId+local SlotKey, revision guards
replacement/release. SetMesh/QueryMesh/ReleaseMesh use the existing Command table and one MeshCommands
Handler. Native MeshStorage owns logical entries and live physical versions; StorageSet owns its lifetime.
Prepared Use is Context-owned, holds the actual backing, and publishes complete rebinding at the existing
Owner safe point. The physical ledger only survives to account for already retired versions; it owns no
second logical resource table. No Runtime business branch or new upload state machine was added.

Candidate reserves existing retirement before preparing the common Buffer Transfer. GpuWorkSender
waits actual Copy/Acquire and publishes the candidate only on success. Failed/cancelled candidates retain
last-good. Allocation release uses real SubmissionRequirements; last logical release/detach cannot free
an accepted Draw. Bound replacement must preserve complete metadata to preserve immutable Graph recipes;
a structural layout/submesh change requires cold detach/recompile. Whole backing size includes necessary
four-byte transfer padding for odd UInt16 triangle counts. No implicit business record limit; optional
resident payload-byte budget counts live and retired versions, distinct from VMA allocator overhead.

Full Request dynamic metadata capacities plus RenderPayload retained backing are budgeted. Cold binding
owns a full asset contract, compared structurally; no Hash identity shortcut. Query returns owned metadata.
Failed typed requests retain exact vector pointer/capacity and original source backing for recovery.

Implementation Release full all-j4-k0/no-work and116/116,128.53s. Following formatting-only production
cleanup and new installed Mesh consumer: all/no-work plus affected3/3,10.88s. Public new headers build
independently C++20/GR-. Mesh native test covers3Queue modes,UInt16/32,exact GPU bytes,pixels64/191,
shared two Contexts,replacement,old revision,logical deletion,generation rollback,accepted Draw retention,
second-submit failure retaining last-good,130 simultaneous records and optional budget exhaustion/retry.
Record/execute/wait window first-party new=0. Runtime test uses2Clients same numeric RequestId, real
upload and query/release, failure recovery,exact budget refund and source Owner after Server/Client death.
Installed relocated SDK consumer performs actual Mesh upload/query/release. Validation/SyncValidation
errors0; actual EOS duplicate warning retained. Independent exact-I ASan checkpoint still pending.

Original failed attempts retained: CheckedArithmetic returns expected (not optional), corrected local
arithmetic chaining; new GPU fixture initially lacked colocated Execution DLL, fixed via existing output
directory convention; Runtime fixture initially lacked Support include, fixed without production changes.
No failure was hidden, disabled or counted as PASS. Old114 test names/assertions preserved.

Legacy719,3 protected user files,dependencies/history/whitelist pass. Exact-I SDK sync follows commit.
Evidence E:/RenderV2Rebuild/V12/evidence/N6-mesh. Root TOOLCHAIN historical BLOCKED; performance/IPC
PARTIAL; upper Engine/Editor/Linux/Android/TSan NOT_RUN; GPU timestamp NOT_MEASURED. No new speed claim.
