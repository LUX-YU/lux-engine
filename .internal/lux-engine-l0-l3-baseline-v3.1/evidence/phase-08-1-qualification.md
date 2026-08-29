# Phase 8.1 Qualification Evidence

## Qualified revisions

- Contract: `9ed0a866`
- Runtime hardening: `2c69b879`
- Deferred/lifetime coverage: `d6f23efd`

## RelWithDebInfo qualification

- Toolchain: MSVC 19.44.35228, Ninja, VS Developer PowerShell.
- Full build command: `cmake --build E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine --target all -- -j 4 -k 0`.
- Consecutive build results: `ninja: no work to do` and `ninja: no work to do`.
- Full CTest result: 108/108 passed.
- Installed consumer: `scene-world-runtime` compiled and ran against the installed public headers.
- Public headers were synchronized to Debug, RelWithDebInfo, and Android install include prefixes before qualification.

The runtime tests cover explicit byte limits, per-submit byte accounting, partition bundle/generation identity,
root/sidecar metadata agreement, deferred completion, stop during pending IO and private decode loops, Scene/requester
destruction, multi-volume, multi-extent, multi-chunk assembly, and mutation-free materializer rejection/rollback.

## Sanitizers

- Windows ASAN build directory: `E:/SyncForder/CodeRepos/build/RelWithDebInfo-ASAN/lux-engine`.
- Compiler/link flags: `/fsanitize=address` and `/INCREMENTAL:NO`.
- ASAN targets: `world_storage_test`, `scene_world_runtime_test`, `scene_world_runtime_deferred_test`.
- ASAN CTest result: 3/3 passed.
- TSAN: not run. The qualified host is Windows/MSVC, which does not provide the required ThreadSanitizer configuration.
  This is recorded as unavailable, not as a passing result; the supported Linux/Clang TSAN run remains an external
  qualification item.
