# N2/N3 joint independent Verification

Implementation I `14d9d3d22953dadc9c9c0698c1bbbeaf6f62d5e9`; parent `97fbd8dbd6638fdc57eeb382df0a9a619d0ae73b`. Detached clean no-hardlinks clone `E:\RenderV2Rebuild\V12\n23-verification`,
no alternates, unchanged throughout verification. Fresh independent build trees n23-build/r,d,a.
Remote observed `78bcd220815f845466d85d31f994901e4db224ba`; no push. This is the joint gate, not final N7 acceptance.

| Configuration | Complete applicable tests | Time |
|---|---|---|
| RelWithDebInfo |106/106|123.69 s|
| Debug |106/106|122.41 s|
| MSVC ASan /Zi |106/106|172.21 s|

Each runs tracked snapshot, fresh configure, all -j4 -- -k0, second no-work and serial CTest -R . .
C++20 /GR-, ASAN_OPTIONS=alloc_dealloc_mismatch=1. Same 106 names in all modes; all original baseline
names retained. Validation+SyncValidation enabled on actual RTX4070Laptop; no validation/sync/ASan errors.
Actual overlay warnings and compiler/config warnings are preserved in full logs, not removed.
Separate synchronous source configure disables stdexec and Vulkan; its full build/no-work/tests pass.
Standalone public heads, pure Client and moved SDK external consumers are included. Exact-I headers
are backed up and synchronized to all three prescribed prefixes; manifest records additions/removals,
original/source/installed SHA256. Android header copy is not Android build qualification.

N2/N3 structural and functional gate PASS within authorized scope. Proof map is in joint I, plus
full raw test-matrix.json. One Command table, one stable Handler owner table, one stable Feature table,
one DeviceSession and one generic Pending owner/event queue remain. Hello now provides only bootstrap;
explicit discovery, cold switching, stale Session/ID and failure rollback are tested. A held old async
operation completes through its original Handler after another Client selects and uses a new Handler.
ConfigValue and metadata own their bytes/pins; presets resolve stable names again after Server restart.
Generic Protocol contains no GPU classification. Native operations preserve real Copy/Acquire/publication
completion and accepted-work lifetime; three Queue topologies, LastUse, partial submit, stop/abandon,
four View output, external Buffer, DLL tails and original request/budget recovery all pass.

Observed layout: active call16B, CommandEntry24B, cold metadata152B, RenderRequest256B;
ConfigValue48B, FeatureUse88B, CreateEffectRequest48B; IpcMessage144B, Reply304B, inline80B.
Four-View steady-frame first-party new=0 is retained in exercised tests. Full raw performance samples
are archived; no speedup or equivalence inference from these tests. Performance remains PARTIAL,
including historical Native/Alias tails. Paired stage performance and VTune attribution remain N7.

Protection PASS: Legacy719 blobs, all three modified user files, dependency commits, old history/checkers,
allowed changed paths. No user worktree changes. Root TOOLCHAIN historical BLOCKED, upper Engine/Editor,
Linux/Android/TSan NOT_RUN. Future Gpu timestamp qualification only where actually measured. N4-N7
NOT_STARTED at this checkpoint. Conflicting Feature replacement is rejected; N5 live-only layout and
parameter/update contracts are not certified by directory tests. N4 owner, capacity and retirement
changes are not claimed complete. This gate permits N4 to start, not final push or product PASS.

Archive `E:\RenderV2Rebuild\V12\archive\14d9d3d22953dadc9c9c0698c1bbbeaf6f62d5e9\n23-joint`; portable `E:\RenderV2Rebuild\V12\archive\14d9d3d22953dadc9c9c0698c1bbbeaf6f62d5e9\n23-joint-evidence.zip`; ZIP SHA256 `185a9f943b8b5a9aa42c1b90e2fa65fb07c69db13cbf34d594ec8eba776e71db`.
Index SHA256 `98bf765666b937d32e2e0fb9eeebbd61246d2f5f4226a1372a14e9f7c58e6783`. Includes commands, exits, warnings,
raw CSV, implementation attempts, SDK backups, protection and all test names. V adds only this report
and evidence index; no production/test/checker edits. Final N7 still requires an exact-I independent run.
