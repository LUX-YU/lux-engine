# Native upload generic completion — independent checkpoint verification

Implementation: `bb386d0e94847765596e3fd0048c7b15e7b58e65`.
Parent: `54de6230e1f6d4a03da12fea8b9daa9c62afa6d5`.
No-hardlinks/no-alternates clean clone: E:/RenderV2Rebuild/V12/n3-native-upload-verification.
Fresh build: E:/RenderV2Rebuild/V12/n3-native-upload-asan.
Runner: E:/RenderV2Rebuild/V12/verify-n3-native-upload.ps1; no verification source edits.

Tracked snapshot, MSVC /fsanitize=address /Zi, all -j 4 -- -k 0, second no-work, all 106/106
Render/direct-dependency/Platform/installed tests passed in 247.49 s, with
ASAN_OPTIONS=alloc_dealloc_mismatch=1. No ASan or GPU Validation/SyncValidation errors.
Actual warnings, command return codes, source paths and raw CSV remain in the SHA-256 evidence index.
Release implementation rerun was 106/106 in 178.78 s. The earlier full 105/106 registration failure
and the failed first-pause assertion are retained, not erased or counted as passed.

Production SetTexture/Region now use the generic deferred Reply and actual Native Transfer sender.
The targeted production test proves a real prior accepted Submit before stop/DeviceLost, original
Payload address/capacity preservation, returned request after Server destruction, exact source release
and budget refund. Old Probe tests, three queue modes, four View pixels, LastUse and callback-tail
cases remain included. The null-State registration rejection was fixed without removing its assertion.

Measured: IpcMessage 144, IpcReply 304, RenderRequest 256, DeferredCompletion 768 bytes;
active call 16/8, hot Command entry 24/8, cold metadata 184 bytes. Cold metadata gained the selected
provider pin borrow; active routing and original Request sizes are unchanged. No speedup/equivalence
claim follows from these sizes or test durations. Full performance attribution remains PARTIAL.
Legacy719, three protected user files and exact dependency state passed protection validation.

This V qualifies only the above exact implementation prerequisite. N2/N3 are still IN_PROGRESS;
remaining old registration, GPU/Readback bridge/classification and Pending migration are not qualified
by this report. Subsequent uncommitted bridge-removal work in the implementation tree is excluded.
No push or N4 entry; root product BLOCKED, upper product NOT_RUN and historical qualifications unchanged.
