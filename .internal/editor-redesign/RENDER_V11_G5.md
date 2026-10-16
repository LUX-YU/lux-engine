# v11 G5 — Queue completion and unified upload

Parent: `82ec60ff572b0608a720dde18ce6c5e20db833ee` (G4).
This is the internal construction gate, not the final independent S1/S2 Verification.

## One execution owner and genuine completion

SubmissionQueue retains its existing native submission/frontier authority. A 48-byte intrusive callback node belongs to
an address-stable operation, reserves one of the queue's bounded callback credits before Submit and can be reused across
chunks. Arming a real accepted ticket does not require another capacity allocation. Poll/begin/wait only advance the
frontier; DeviceSession::dispatchCompletions is the explicit owner-thread safe point. Dispatch detaches its ready prefix,
unarms each node, caches its function/user/next and never accesses that node after invoking the callback. A rearmed node
waits for a later dispatch. Ordinary Fence errors do not authorize resource release. DeviceLost is a distinct terminal.

The existing Server Impl owns the only fixed-capacity DeferredCompletion slots. CPU notifications and GPU callback nodes
refer to those same slots. Tick no longer scans pending GPU/Readback work. Readback reserves its node and original request,
uses a real execution/capacity ticket, and cancels its Context borrow before topology retirement. A topology operation may
scan the bounded set to cancel matching readbacks; shutdown may snapshot at most 136 identities. Neither is completion
polling on a stable frame. Shutdown cancels future work, drains already accepted GPU obligations, consumes CPU/queue events,
waits publication tails and settles every ReceivedRequest before resetting the Responder. No new queue system or worker
was added, and the existing one-millisecond host sleep is retained.

GpuWork now owns the actual Transfer plus a professional publication/rollback candidate; it no longer erases arbitrary
advance/lastUse drivers. A candidate owns no Transfer or second upload phase machine. render::upload dispatches through
StorageTraits for both production TextureStorage and the independently implemented external TestBufferStorage. Texture
format/Mip/BC/Region/revision/descriptor publication stays in TextureStorage. Generic Buffer/Image copies, CPU staging,
chunking, queue-family release/acquire, real tickets and query statistics stay in Transfer. TextureUpload.hpp/.cpp and the
old UploadState.hpp are actually removed. DeviceSession's one staging/page/budget/query-pool ledger is private in
Transfer.cpp; its capabilities are retained. Historical TextureUpload.cpp is now only the equivalent regression test name.

## LastUse and lifetime

Backing-owned SubmissionRequirements is a fixed 48-byte value with at most three physical queue conditions. Same-owner
serials merge by maximum; different queues remain independent. Copy/clone and Draw merge at actual Submit acceptance.
Callbacks cannot overwrite a newer Draw with an older Transfer ticket. PreparedStorageBinding still borrows one stable
requirements pointer; its layout remains 64 bytes and ExecutionContext::Binding remains 32 bytes. RetirementEntry remains
72 bytes and uses the same conditions. Permit release verifies Session/SlotKey/queue ownership before reading completion;
no stale Allocation pointer is used to identify released objects.

The targeted real-GPU test observes T1 Clone only while T2 Draw is held, publishes/replaces/deletes, and proves old
backing/view/descriptor retention until T2. The multi-chunk case holds the second CPU preparation, cancels without a T3
that could conceal regression, and checks the original binding still contains T2. Real queue configurations are alias
0:0, same-family 0:2 and dedicated 1:0 on the current device. Region itself deliberately uses Graphics because its original
sampled image and Clone transitions share that owner; the report does not label that copy a dedicated-transfer Region.
Actual VMA allocation/range and descriptor counts are measured; no independent Arena exists in this path.

Readback's extracted RenderPayload now explicitly retains Server implementation code. The new DLL test retains only the
payload after Client/Reply/Server destruction, checks exact byte preservation and releases the module once after the
final owner destructor returns. Its first failure was an incorrect new exact UNORM oracle: both before/after were
32,64,127,255 and the whole buffer was identical. The corrected test uses the existing ±1 GPU quantization oracle before
teardown and an exact whole-buffer comparison afterward. Failed diagnostic logs are retained; no production color rule
was changed to make that test pass.

## Qualification

The initial full build exposed missing Native forward declaration, old fixture LastUse type and incomplete Impl static
function-table placement. Those are repaired; the failed builds remain archived. The original 79 names/valid behavior
are retained. Additional G5 tests are render.native.queue_callbacks and render.native.last_use.

Final matrix and evidence index are appended only after actual execution. G6 still owns final performance/SDK cleanup,
independent clean-clone V and push. Historical IPC R2 PARTIAL, Root TOOLCHAIN status and untested platforms are not changed.

### Completed local gate matrix

RelWithDebInfo 96/96 (92.30 s), Debug 96/96 (96.06 s), MSVC ASan 96/96 (126.07 s).
Each configuration ran full all -j 4 -- -k 0, repeated with no work, followed by complete CTest -j 1 -R .
ASAN_OPTIONS=alloc_dealloc_mismatch=1. Validation/SyncValidation and ASan errors: zero.
Existing duplicate EOS overlay warnings are retained. Public-header and relocated install tests are included.
Protection passed for all 719 Legacy blobs, histories, original user changes, dependency versions and physical IPC.

Measured Binding=32 B, PreparedStorageBinding=64 B, RetirementEntry=72 B, SubmissionRequirements=48 B,
SubmissionCallback=48 B, GpuWork=72 B. The 10k-record case remains 1,343,640 bytes / 61 cold allocations.
Texture upload wall measurements now also include Storage/descriptor publication and readback/release; they are not
workload-equivalent to the historical raw-upload wall number and do not support a speedup claim.

Evidence index: E:/RenderV2Rebuild/V11/evidence/g5-evidence-index.json (156 files), SHA-256
457c7a21d4c62d5c6aeaded22284dcec7108b92d8c2dfe020f64fc6052a626ad.
G5 internal gate PASS. Final S1/S2 independent qualification remains pending G6.
