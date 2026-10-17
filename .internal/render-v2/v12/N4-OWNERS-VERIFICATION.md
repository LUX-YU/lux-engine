# N4 actual owners and physical retirement — independent Verification

Implementation `221b70f9b3434a5469988e08803ba9b3bfad1649`, parent `1dcef93b9156b317b3c2aa068ae86f0d7691e3c4`; remote observed `78bcd220815f845466d85d31f994901e4db224ba`. No push.
Independent `git clone --no-hardlinks --no-checkout`, detached exact I at `E:\RenderV2Rebuild\V12\n4-owner-v2-verification`, no alternates,
clean tracked snapshot before configuration, fresh independent r/d/a trees. No verification source edits.

## Matrix and commands

Windows/MSVC C++20 /GR-, RTX4070 Laptop GPU. RelWithDebInfo 106/106 in
123.39s; Debug106/106 in131.05s; MSVC ASan /Zi106/106 in
166.17s (`ASAN_OPTIONS=alloc_dealloc_mismatch=1`). Each uses the archived runner:
snapshot, configure, `cmake --build <build> --target all -j 4 -- -k 0` twice (second no work),
`ctest --test-dir <build> --output-on-failure -j 1 -R .`. All102 baseline test names retained.
Validation+SyncValidation enabled, errors0; ASan errors0. Existing overlay warnings are retained in logs.
Public headers, installed/relocated consumers and external Buffer run in all matrices. Three SDK include
prefixes copied from this exact I with before/after hashes and backups; Android header sync is not a build.

## Gate result

N4.1–N4.6 correctness PASS within the tested scope. Registry is the sole StorageId allocator and borrows
instances; actual Device/Scene/View/Context StorageSets own them. Qualified resource handles, preload vs
Attach leases, rollback, stale identities and exact detach ordering pass. ExecutionGroup remains four fields.
ResourceDomain/Scope/ancestor lookup, Registry instance ownership and Factory.collect are deleted.

The original RetirementQueue owns publication-time reserved entries. Physical Queue frontiers and ready
entries drive release; no live-Storage/backing scan. Texture/Buffer logical owners can end while accepted
GPU work retains backing and descriptor rights. Real Ticket/LastUse, partial-submit, three topologies,
replacement, shutdown and delayed T1/T2 regressions pass. New two-Scene sampled-image test proves
separate StorageIds/slots and exact independent pixels32/96 while sharing one physical descriptor table.
One Scene can disappear while the other continues; final resource destruction follows real completion.

Default Texture growth129 real uploads/readbacks passes. Explicit capacity/byte-budget negative tests
remain. Physical descriptor capacity is the actual device limit (1048576 on this GPU), not universal.
1/100/1000/10000 real live VkBuffers with prepared retirement rights,1000 zero-retirement polls each:
zero candidate visits and zero engine allocations. These buffers were not submitted; this measures idle
retirement scaling, not rendering throughput. Diagnostic samples are archived, no speedup claim.

Measured release sizes: Binding32, Context264, RetirementEntry104, PreparedBinding64, StorageUse88,
Registry168, StorageSet40, Factory176, SubmissionRequirements48, SubmissionCallback48,
TextureBacking568, GpuWork40 bytes. Host10000 record+binding allocation1343640bytes/61 new calls.
The earlier candidate failed Debug zero-allocation assertions; the correction removes whole-Record
move construction and its checked-string proxy allocation. Original assertions remain; both attempts
and controlled reproduction are archived. N5 must still replace fixed ShaderBinding layout, typed Attach encoding, external Buffer private descriptor
shape, last-provider float4 and mandatory readback; N4 does not certify these future capabilities.

## Protection and limits

Legacy719 blobs, main/Layer user-file hashes, dependencies, historical reports/checkers and whitelist PASS.
Owner teardown requires outstanding asynchronous borrowers to be terminal before their StorageSet ends
(v12 §9.6.6); no implicit Owner-lifetime manager was added. Performance remains PARTIAL. Root TOOLCHAIN
is historical BLOCKED, upper Engine/Editor/Linux/Android/TSan NOT_RUN. GPU timestamp/cross-version
equivalence are not inferred from these correctness runs. N5–N7 not yet qualified.

All commands/exits, samples, SDK backups and implementation failed attempts are indexed under `E:\RenderV2Rebuild\V12\archive\221b70f9b3434a5469988e08803ba9b3bfad1649\n4-owners`.
Portable archive `E:\RenderV2Rebuild\V12\archive\221b70f9b3434a5469988e08803ba9b3bfad1649\n4-owners-evidence.zip`, SHA256 `e144a9231cadb7f2dcddbe38169681e19f7e5c587ee11a28f0693f833c5f22cd`. V only adds this report and the SHA-256 index.
