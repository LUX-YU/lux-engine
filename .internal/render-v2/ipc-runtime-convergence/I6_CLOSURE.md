# I6 — public surfaces and deletion closure

I5: a4d69e63f (full commit identity in final index). No additional Owner, queue, registry or execution pool is introduced by cleanup.

The new contract checker scans production IPC/Render surfaces, verifies removal of RenderClient, CommandValue, TypedProtocol and MessageAccess, rejects the superseded completion/binding/budget/pin symbols, and checks the neutral Server and immutable StorageFactory. It is separate from all historical checkers. Current result: 124 production source/header files checked; PASS. The protection checker still verifies 719 frozen blobs and four original user files.

The neutral Server header probe now compiles using only Commands/Features/Resource dependencies, without obtaining Native include paths by linking Runtime. The selected Vulkan bootstrap has its own independent header probe. SDK consumers require its installed header and continue checking absent legacy/private headers. The pure Client configuration still disables both Vulkan and stdexec discovery.

The old Payload assignment reset existed to order module pin release. With pins removed, assignment delegates replacement to the existing SharedBytes owner directly. Empty moved-from views and complete backing size semantics remain unchanged. No data Owner or deleter is replaced.

The running basic_mesh example uses raw IPC and caller RequestIds, a real ListCommands ID from Host, the selected Vulkan bootstrap and one Host ExecutionRuntime. Its active README reflects this usage; historical verification reports remain unchanged.

## Owner and commit points

| Object | Owner / release condition |
|---|---|
| Input/output object | IpcMessage, move/destroy through its private static TypeOps; Host keeps code loaded |
| Segmented SPSC | Existing Link; published slots remain exclusive; returned empty segments can be reused |
| Accepted business request | RenderRequest / its promoted stable DeferredCompletion, retaining original ReceivedRequest source |
| Failed reply publication | Existing ready result + source; retry only publication, never execute Handler again |
| Command / Handler | Existing Command SlotMap borrows active invoke/object; existing Handler SlotMap uniquely owns object |
| Storage and Native | Existing StorageSets/Registry index/DeviceSession; GPU Ticket and LastUse remain authoritative |
| CPU scheduling | Host ExecutionRuntime owns pool; Impl borrows its CpuScheduler |
| Foreign code lifetime | Host unloads after messages/configs/handlers/operations and callback stacks end |

Source gate and I5 focused construction checks are not a substitute for final exact-I independent verification. I7 performs that verification once, retaining baseline Debug failures as separate facts and unresolved Native/Alias/IPC performance as PARTIAL.
