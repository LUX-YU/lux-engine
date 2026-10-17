# N5 layout and rebinding — independent ownership checkpoint

Implementation `1f499e4638f66577d03b9a3078d77c66e211d9d6`, parent `ec2c34fb197d0c023acb1012efb0628dabc46e94`. Remote tracking observed `78bcd220815f845466d85d31f994901e4db224ba`; no push.
Exact detached clean clone `E:\RenderV2Rebuild\V12\n5-layout-verification`, --no-hardlinks, no alternates; tracked snapshot before configure.
Verification source remained unchanged. V adds only this report and the evidence index.

Independent Windows/MSVC ASan /fsanitize=address /Zi, C++20 /GR-, alloc_dealloc_mismatch=1:
**107/107**, 167.06s. Fresh configure, all -j4 -- -k0, second no-work, then CTest -j1 -R .
All102 baseline names preserved. Includes installed/relocated public SDK, pure Client and external Buffer
consumer, Native/Runtime/IPC regression, real GPU Validation/SyncValidation. Errors0, ASan errors0;
actual warnings retained in raw logs. No new full independent Debug/Release claim for this exact I.

Actual checks: private fields packed into one set; provisional set2/3/6 and binding5/7/11 relocated;
same/distinct Buffer resources produce32/16 RGBA pixels; names/shape/stage/source positions reject cold;
shared table complete shape remains device-owned. Storage no longer dictates placement or owns private
buffer descriptor pools. Pool/set failure rolls back before publication. Four existing Runtime Contexts
adopt replacement without reattach; Readback may return an already-submitted old frame until a new draw.
Old snapshot assertions were explicitly migrated, never removed to hide an unexplained failure.
Repeated updates avoid the bank of the held incomplete Ticket. Physical backing/Context retirement,
LastUse, zero-hot-allocation and R5 pipeline/bind/scope checks remain. Final release has no new allocation.

Implementation full107/107 in123.17s before final distinct-resource/fault additions and demo-use cap removal;
final all/no-work and targeted4/4 in2.25s. Earlier105/107 obsolete-contract attempt retained in evidence.
Recorded sizeof Binding32, Context296, PreparedBinding80, StorageUse104, RetirementEntry104 bytes;
10k host ledger+Binding1,343,640 bytes/61 allocations. Descriptor banks cost additional cold GPU storage;
no performance improvement or whole-renderer memory-saving claim.

Protection719 Legacy blobs, three user files, dependencies and history passed. Exact-I headers backed up
and synced to all three prefixes; Android sync is not a build. Structural traceability is not qualification.
**Current layout/rebinding subset PASS; full N5 IN_PROGRESS.** General parameter/Shader ABI, lglsl asset
integration, Depth/Index/Indirect/Compute and final combined gates remain. N6/N7 pending. Performance
PARTIAL, full product historical BLOCKED; upper Engine/Editor/Linux/Android/TSan NOT_RUN.

Portable raw evidence `E:\RenderV2Rebuild\V12\archive\1f499e4638f66577d03b9a3078d77c66e211d9d6\n5-layout-evidence.zip`; SHA-256 `fcd4231bc59e036886c68b2012114101697fc7dc802e6fc65ada88faca14c862`. Per-file hashes in adjacent index.
Final N7 still requires all three complete configurations and equivalent workload performance measurements.
