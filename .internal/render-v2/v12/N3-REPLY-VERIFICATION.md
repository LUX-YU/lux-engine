# Typed Reply prerequisite — independent verification

Implementation: `25f54adc812828c91e1f376a5da2fbeffab259a9`.
Parent: `e28a1f458a3dc5b8c99b09a47199ef51357c6c2a`.
Independent no-hardlinks/no-alternates clean clone: E:/RenderV2Rebuild/V12/n3-reply-verification.
No verification source changes. Tracked snapshot, fresh MSVC ASan /fsanitize=address /Zi,
full all -j 4 -- -k 0, second no-work and 105/105 tests passed (253.05 seconds).
ASAN_OPTIONS=alloc_dealloc_mismatch=1. No ASan or GPU Validation/SyncValidation errors;
actual warnings are retained. N3-REPLY-evidence-index.json provides raw log SHA-256 values.
Runner: E:/RenderV2Rebuild/V12/verify-n3-reply.ps1.

Tests include explicit SBO promotion/reborrow, rejected promotion preserving the input,
dropped deferred Reply cancellation, exact payload destruction and budget refund,
0/1/16/128 completions, cross-DLL callback tails, shutdown and all prior GPU/Platform behavior.
Legacy719, the three user files and dependencies passed the protection checker.
Sizes: HandlerCall 16/8, CommandEntry 24/8, metadata 144, RenderRequest 256 bytes.

This closes only the initial Reply prerequisite. N2/N3 remain IN_PROGRESS: Handler ownership,
installation and the old GPU completion bridge/condition variant must still converge.
No performance PASS, product qualification or push is implied.
