# N5 explicit Shader stage/entry — independent checkpoint

Implementation `fc9c0f1adbd0be0f5b60f6e704667b22c8775997`, parent `53e13fb16c51a789fcb6a8816795e0df06efc328`. Observed remote `78bcd220815f845466d85d31f994901e4db224ba`; no push.
Exact detached clean clone `E:\RenderV2Rebuild\V12\n5-stages-verification`, no-hardlinks, no alternates, tracked snapshot before configure.
Verification source unchanged. Later uncommitted LayoutPlan work is excluded from this qualification.

Independent Windows/MSVC RelWithDebInfo C++20 /GR-: **107/107**, 125.54s.
Fresh configure, all -j 4 -- -k 0, second no-work, CTest -j 1 -R .; all102 baseline names preserved.
Includes public headers, fresh/relocated SDK, pure Client and external Buffer consumers.
Validation/SyncValidation errors0; warnings retained. No new Debug/ASan claim in this checkpoint.

Explicit stage/entry metadata drives real pipeline creation. Test uses non-main entry names, reversed
stage order and real submitted pixel readback; invalid entry/stage combinations reject cold.
Complete shader identity includes metadata and words. Data consumes existing neutral metadata headers;
no Vulkan/Engine implementation dependency is introduced. Legacy719/user/dependency/history protection pass.
Exact-I public headers backed up and synced to three prefixes; Android header sync is not a build.

**Stage/entry subset PASS; full N5 IN_PROGRESS.** Layout, complete ABI, parameters, primitives and full
rebinding remain. Performance PARTIAL; product historical BLOCKED; upper Engine/Editor, Linux/Android,
TSan NOT_RUN. No performance improvement claim. N7 still requires all three full configurations.

Raw evidence `E:\RenderV2Rebuild\V12\archive\fc9c0f1adbd0be0f5b60f6e704667b22c8775997\n5-stages-evidence.zip`, SHA-256 `a48f6d3bf72fe73c2c55ca1ca31663dc9bf69001113ebcd0ed4e6771575d2403`. Per-file hashes in adjacent index.
