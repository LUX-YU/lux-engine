# S0/G10 ownership and commit points

Baseline: be9d7af3d4378a1115d9373bc4e73bc5d7bb68c2. Specification SHA256:
44b6c2c258c0a64a081c718482b50da35037a6cc0803a5466770cfec91b50e38.

DeviceSession alone owns Device/VMA and the stable allocation ledgers. Context bindings borrow records;
the Context unique_ptr and scoped lease counts already prove internal destruction after its last Ticket.
External raw-pointer retirement currently provides no exclusive release right and can admit a duplicate.
S0 adds only a move-only AllocationPermit<Tag> (Session + existing SlotKey) and an allocation retired bit.
External creation returns the permit; cold borrow resolves it through its Session before dereference.
Admission checks owner, generation, active state, queue Ticket and retirement capacity before consuming it.
Only RetirementQueue, Context and DeviceSession retain the private trusted destruction path. Unsubmitted
candidate destruction is explicit. Alias references keep the existing physical allocation pin algorithm.
Binding and the composite Context retirement path do not acquire another key, permit or owner.

RenderServer::Impl owns the only Operation SlotMap, Scenes, Effects, DeviceSession, compile slots and
readback requests. Each RenderRequest owns its original IPC ReceivedRequest and borrows the frozen
descriptor/Responder. Ordinary reply currently permits an unproven GPU-ready success: S0 rejects that
contract both at registration and at reply. Zero-pass contributions skip Program; live passes still require it.
Client binding fields become private with value/const accessors; raw IPC tests retain server boundary checks.

Only after S0 verification: G10 needs fixed-capacity deferred slots, a synchronous borrowed OperationContext
and a move-only slot reservation. Existing readback responsibility will share that pending owner with real
GPU Ticket completion. Reserve reply space and a slot before submission; commit transfers the original
request and required pins with no further capacity admission. Tick polls; shutdown joins compile and
settles pending source qualifications before Responder reset and Device destruction. No new resource table,
operation table, scheduler or IPC algorithm. Exact API details follow the actual upload consumer.

Read-only: all Legacy, IPC, G4/G5 algorithms, G2/Protocol, historical reports/checkers and primary worktree.
S0 edits: Native Memory/DeviceSession/Retirement/Execution private creation seam; Runtime Client/Server/Request;
their affected Foundation, Programs, FeatureOperations and fixture tests; new .internal S0/G10 material.
G10 may extend only these Runtime/Native seams and add targeted tests/build/header installation checks.
