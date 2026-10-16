# v11 G2 — typed Command and implementation binding

Construction parent: `360c895236350fd5ba65c7b57106d168449c9a16` (G1). This is an internal gate,
not the final S1/S2 independent qualification. No S2 mechanism is claimed here.

## Authority and migration

The original Server SlotMap is now the only Command table. A static Command declaration supplies its complete
name, version, Request/Response type and layout and completion contract. Cold registration compares that declaration
with its handler and canonical IPC TypeOps. Client binding makes an immutable capability; ordinary dispatch uses its
CommandId and canonical object qualification. Cross-DLL Hello performs the full cold ABI comparison.

All existing Scene/View/Target/Effect/Readback/Stop calls and the new small Stats query use constructed C++ values.
Texture declares SetTexture, SetTextureRegion, QueryTexture and ReleaseTexture; their actual handlers and the external
TestBuffer provider live in Native. Native has no Runtime/IPC include dependency. Feature/Storage registration is a
neutral cold binding, not another directory. Runtime owns the original ReceivedRequest and terminal publication.

Operations.hpp, Operations.cpp, Runtime Wire.hpp/Wire.cpp and the old Runtime Texture handler are removed.
Author configuration, Storage configuration and Binding still consume the existing neutral Schema/Wire contract.
RenderPayload moved to Data without an alias; its asset subrange retains and charges the original complete capacity.

## Ownership and budgets

One ValueEnvelope contains the concrete value and a slice of the Client's existing budget. Value destruction precedes
refund. Request capacity, strings including terminators, complete asset backing, Response capacity and actual envelope
padding are charged before moving the input. Rejected publication reconstructs the original Request in place, including
vector address, content and capacity. An accepted Reply owns both original input and result; retaining it retains budget.
Explicitly taking a value out of a consumed completion transfers it to application ownership.

An uncommitted S1 GPU reservation borrows the handler's stack request and must commit or roll back before that call
returns. Only the existing committed stable Deferred slot may outlive the call. The final S2 handoff will replace this
transition with the single bounded asynchronous owner. GPU-ready ordinary completion remains forbidden. An unsent GPU
result is destroyed before publishing a failure or shutdown terminal that can return its original budget reservation.

Type binding is a copyable value qualification, including when passed an rvalue: the source cannot retain a DLL TypeOps
pointer after losing its pin. StorageRegistration has no whole-object assignment; it is constructed once, so assignment
cannot release a factory's code pin before destroying its previous foreign configuration.

## Evidence and remaining gate work

Raw intermediate build, test, CSV and failure evidence is under `E:/RenderV2Rebuild/V11/evidence/g2-*`.
The round-5 RelWithDebInfo full build, second no-work build and all 86 tests passed. All 79 original names remain.
New coverage includes typed ABI/Command contracts, full backing retention, two-Scene vector batch reuse, cross-DLL
canonical types and original Request return, malformed Reply identity/outcome, and real GPU-ready failure ownership.
Batch instrumentation reports actual allocations separately from same-type STL move costs; it does not relabel Debug
vector bookkeeping as zero total allocation. Original GPU tests still execute their three physical queue configurations.

Subsequent audit reproduced a real lifetime defect in the initial STATIC Client: a Client created in an unloadable DLL
left a late Reply holding that DLL's BudgetState control block. After actual unload, final completion destruction faulted.
The exact failure is archived in `g2-dll-repro`. Client is now one real shared library, so its budget control block,
manifest and slot implementation remain in that common library while an application plugin unloads. Class-specific delete
pairs the exported factory allocation with the same DLL. Test-only allocation instrumentation separately counts this
DLL and the EXE. The reproducer now passes after actual plugin unload and late completion destruction; this does not add
a per-message implementation pin or a second budget owner. A server-plugin TypeOps pin test alone did not cover this
distinct Client implementation lifetime.

The final G2 RelWithDebInfo and Debug builds, second no-work checks and full 86-test runs pass. Their complete logs are
under `g2-final` and `g2-final-debug`. The first shared-Client full run exposed an outdated Hello fixture pointer comparison;
the fixture now performs complete cold ABI binding for Hello, while ordinary bound requests retain canonical O(1)
qualification checks. Its failed run remains under `g2-shared-client`. ASan also passed all 86 tests with
`alloc_dealloc_mismatch=1`, under `g2-final-asan`.

A further real Server-plugin test then reproduced a distinct missing implementation pin: after a plugin-hosted Server
and Client were destroyed and the plugin actually unloaded, destroying a retained builtin CreateScene Reply faulted.
The no-pin failure and binaries are preserved under `g2-server-dll-repro`. The host now supplies its implementation pin
in ServerConfig (empty explicitly means process-lifetime code); builtin, Hello and error publication retain it through
the existing IPC qualification. Error TypeOps are bound once cold and compared by canonical qualification thereafter.
The independent Server plugin test passes for late success, Hello and raw-peer error Replies; each case proves actual
unload exactly once, after the final message destructor returns. The host keeps its own pin across factory/run/destruction.

Asset byte owners retain their original deleter. A separate Asset DLL repro faulted after its last module reference was
released while a byte Owner remained (`g2-owner-dll-repro`). RenderPayload therefore carries an optional source code pin
independent of Command code, without creating a second byte owner. Copy/move assignment destroys old bytes before old
code can unload. The new test checks two original-address subranges, full capacity charge, rejected ranges, copying and
moving, then the final Owner ending via destruction or either assignment. RenderPayload is now 48 B. Its bytes() result
is borrowed under the full Payload's lifetime; copying bytes alone does not transfer the separate module qualification.

Final internal G2 matrix after all repairs: RelWithDebInfo, Debug and ASan each pass full all build, second no-work build
and 87/87 tests. The original 79 names are preserved. Logs and raw CSV are in `g2-pinned-{release,debug,asan}`; ASan retains
`alloc_dealloc_mismatch=1`. This is an implementation-worktree gate, not the final independent clone V.
The 175-file evidence index is `E:/RenderV2Rebuild/V11/evidence/g2-evidence-index.json`, SHA-256
`d94abfe8a78ca92a2f4adf4b40e53d27a0d39539a7078d6160ab715084d684d0`.

G3 still removes transitional IPC byte adapters and selects the measured final SBO layout. S2's existing CompileJob.done,
Deferred polling and TextureUpload remain unchanged until their gates. Legacy 719, user modifications, dependency SHAs,
historical reports and IPC R2 PARTIAL remain protected. Final independent clone verification and push remain pending.

G2 internal gate: PASS. S1 cleanup and S2: NOT_COMPLETE.
