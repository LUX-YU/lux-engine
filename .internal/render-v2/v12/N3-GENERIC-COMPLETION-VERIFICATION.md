# Independent generic completion / Native Readback checkpoint V

Implementation I: `3df7ad7b04aaf2f694b9ce149e1fed5bdc1deeda`. Parent: `6fa205e14bd198cd5489465cc2c4e7c03bcfda50`.
Independent source: E:\RenderV2Rebuild\V12\n3-generic-completion-verification. No-hardlinks clone, no alternates, detached exact I, clean tracked source.
Independent fresh ASan build: E:/RenderV2Rebuild/V12/n3-generic-completion-asan.
Runner: E:/RenderV2Rebuild/V12/verify-n3-generic-completion.ps1.

Tracked snapshot, fresh configure, complete all -j 4 -- -k 0, second no-work, and all 106/106
Render/direct-dependency/Platform/installed tests passed in 269.57 s. MSVC C++20 /GR-, /fsanitize=address
/Zi and ASAN_OPTIONS=alloc_dealloc_mismatch=1 were used. Validation/SyncValidation Error=0 and ASan
Error=0; actual environment warnings remain in raw logs. No production or test source was edited in V.
Implementation-tree RelWithDebInfo final all 106/106 passed in 212.41 s. Its earlier 105/106 obsolete
SDK-header failure and build failures are preserved, not counted as success or removed from evidence.

The exact I removes the generic GPU completion category/extension and specialized Pending variant.
Native Readback cancellation no longer scans Runtime pending records. Its actual Queue callback survives
Context detach/destruction until the real terminal event; retirement refusal preserves the original use.
Independent tests cover shared output, reservation-full before Submit, retirement-full refusal, pre-start
stop, observed Fence without callback publication, source return/budget, three Queue topologies, actual
GPU pixels, LastUse, failed subsequent Submit, Client stop/abandon, code pin and late DLL destruction.
The retired GPU-category registration tests have explicit v12 replacements in the implementation note.
Actual GPU-ready success assertions remain based on real producers and Native output, never a CPU flag.

Measured layout at I: IpcMessage 144, IpcReply 304, RenderRequest 256, DeferredCompletion 672,
CommandInvocation 24, SubmissionCallback 48, BufferPermit 16 bytes. These are layout observations, not
performance equivalence. Changed raw-GPU fixture timing is not an equal-work old/new comparison.
Performance remains PARTIAL. Legacy719 and the three protected user files/dependency snapshots passed.
Root product remains historical BLOCKED; upper Engine/Editor tests, Linux/Android/TSan are NOT_RUN.
GPU timestamp is NOT_MEASURED in the raw-GPU cost fixture. Final N7 full matrix/SDK sync is still required.

Qualification: this exact prerequisite checkpoint PASS; joint N2/N3 IN_PROGRESS; N4-N7 NOT_STARTED.
The implementation worktree now contains later uncommitted Command-directory/version changes. They are
explicitly excluded from this V. No push has occurred. V adds only this report and its evidence index.

Immutable raw archive: E:\RenderV2Rebuild\V12\archive\3df7ad7b04aaf2f694b9ce149e1fed5bdc1deeda\generic-completion
Portable ZIP: E:\RenderV2Rebuild\V12\archive\3df7ad7b04aaf2f694b9ce149e1fed5bdc1deeda\generic-completion-evidence.zip
ZIP SHA-256: `2d3cb34d421b4cb35989ce161cbf3b0bb8017588eacd0a8fe5731990717a9178`
Index: N3-GENERIC-COMPLETION-evidence-index.json; SHA-256 `4cf5c2582ca2a845ae97d21f3b8aef139b510ceae59a65b5667dd47385de1055`.
Every archived file has its size and SHA-256 in the index, including scripts, failed attempts, logs,
raw CSV and protection results. Source/dependency/SDK/compiler paths and command return codes are retained.
