# N2 immutable configuration prerequisite — independent verification

Implementation I: `d8a91def0a1a8a602c6fe4bd6efa814830266651`.
Parent: `73535ed6499db8db2904178330be6aed73c66e85` (N2.1 verification).
Independent source `E:/RenderV2Rebuild/V12/n2-config-verification` is a clean detached clone,
created with --no-hardlinks and without alternates. No verification source edits occurred.

Tracked snapshot, fresh configuration, full all -j 4 -- -k 0, second no-work and all 105/105
CTest cases passed (148.85 seconds). This was MSVC ASan /fsanitize=address /Zi with
ASAN_OPTIONS=alloc_dealloc_mismatch=1. It includes the actual external Config DLL lifetime,
relocated installed Data header/consumer and original GPU/Platform regressions. ASan and
GPU Validation/SyncValidation errors were zero; warning output is retained in the raw logs.
ConfigValue is 48 bytes, alignment 8 on this ABI (one static type qualification pointer).

Protection passed for Legacy 719, all three user files and dependency versions. The exact commands,
returns and raw logs are indexed with SHA-256 in N2-CONFIG-evidence-index.json. Runner:
E:/RenderV2Rebuild/V12/verify-n2-config.ps1. This closes the affected configuration prerequisite,
not N2/N3: Handler ownership, typed Reply, installation transaction and remaining Feature migration
are still open. No performance claim, product qualification or push is made by this report.
