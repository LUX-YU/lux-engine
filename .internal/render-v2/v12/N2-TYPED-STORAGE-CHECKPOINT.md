# N2 typed Storage/Handler installation checkpoint

N2/N3 remains IN_PROGRESS. Storage installation now takes its typed Declaration and selected Native
StorageProvider, creates or reuses the real device-borrowing Handler, and obtains Commands from its
member tuple. The old public StorageRegistration aggregate and nativeStorageBinding are deleted.
TextureCommands contains the actual former free-function bodies (four methods, one Device borrow);
external BufferCommands uses the same API without changing Server business dispatch. Handler-free
Storage is supported by an actual graph-only Buffer fixture, without a dummy Handler.

The unique Handler table owns each object once. The sole Registry admits the Factory/config before
infallible publication of new Command support. Preflight validates the declaration, Native type,
Config/Ops types and all member contracts. Reinstalling the same type/key/config is idempotent; another
key adds a supporter without stealing active. Incompatible implementation/config or failed Command
qualification leaves the old directory unchanged. All Factory callbacks are compared for repeat install.
A source Code Pin protects the calling module's canonical TypeOps/config and Handler deleter; Registry
retains the Factory code separately. A returned directory and Client cache can outlive Server; a new
counter test proves the last source pin releases exactly once after these owners finish.

CommandHandler accepts typed Result-returning members to preserve real recoverable errors without
another forwarding Handler. Original typed void members remain valid. No IPC/SPSC/Queue/Transfer,
GPU retirement, request/Pending or Graph algorithm changes. The old Feature-only null Native binding
still exists and must be removed in the next joint migration; this checkpoint does not close N2/N3.

Build: full all -j 4 -- -k 0, second no-work; focused 9/9, then complete 106/106 (0.11 s).
Public-header/installed pure Client and relocated external Buffer consumers are included. The first
attempt exposed real old Storage consumers and a Vulkan header leak; both were fixed in implementation,
with all failed logs retained. No test or header-dependency assertion disabled. Legacy719/three user
files/history/dependencies pass. Performance remains PARTIAL, root product historical BLOCKED;
upper products/unrun platforms NOT_RUN. Independent checkpoint V and final N7 remain required.
Evidence: E:/RenderV2Rebuild/V12/evidence/N2/typed-storage and typed-storage-*.console.log.
