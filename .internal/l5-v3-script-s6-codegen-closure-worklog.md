# S6 build-time codegen correction work log

## Status and identities

- IN PROGRESS, NOT QUALIFIED. No framework freeze, main merge or push is authorized.
- Bwork: `05cb8fde4e1e39bee03595df2559cb60c2b7e2f3`.
- Bsafe: `b03cd8c043773565a79a4de6a91d0a6b3fbe6fa7`.
- Engine branch: `codex/s6-deep-optimization`; main's five edits remain excluded and untouched.
- Dependency baseline: lux-cxx `7716c087e384d1bedc8a4a17a2d6ecb01ed30344`.
- Dependency worktree: `lux-cxx-script-codegen`, branch `codex/script-codegen-closure`.
- New order: P0 baseline, P1 actual generated C++ vertical, P2 migration/authoring/storage,
  P3 prepared Event/result owner, P4 finite Lua/graph closure and final qualification.

## Disposition

- KEEP: W0 ownership/pending/producer/command safety fixes; bounded direct class allocation;
  one TaskGraph, execution owner, HookChannel, stable resume, continuation and ingress contracts.
- REPLACE: CppStatic reflection projection, reflected/typed matcher, arity fallback and prepare-time
  fixed ownership layout calculations with Engine-authored templates and actual typed operations.
- DELETE: unintegrated SuspensionFrameLiveness.hpp. Existing AOT lowering is unchanged.
- NARROW: actual-class metadata and semantic population/budget inputs; no arbitrary page reclassification.
- REQUIRED: generated-source consumers, incremental dependency proof, prepared Event to final owner,
  Lua safe factory reuse, machine-code and memory accounting, final clean qualification.
- DEFERRED BY AUTHORIZATION: FlowForge liveness, arguments/frame merging, provider-bound plan caches,
  public batch APIs, new result compression, command trackers, PGO, VM native entry/allocator/pooling.
- GUI, R1 and new runtime managers are not in scope.

## P0 baseline

- 2026-09-05: Bwork all-j4-k0 build succeeds using MSVC Developer environment.
- Focused CTest: 26/26 PASS, including C++ prepared/coroutine, LuaJIT on/off, continuation,
  Event wait, lifecycle, Hook/Channel, Scene and benchmark smoke.
- First test invocation lacked the MSVC environment: 24/26; two negative compile tests failed
  because the standard `chrono` header was unavailable. That run is not a semantic failure or PASS.
  The complete 26-test selection was rerun in the proper environment and passed.
- Local logs: `build/RelWithDebInfo/deep-developer/p0-codegen-baseline-all.log` and
  `p0-codegen-baseline-tests-msvc.log`. These are development evidence, not final clean qualification.

## Remaining gates

P1-P4, full profile/install/relocation, codegen target/incremental proof and final paired performance
are NOT DONE. Historical checkpoints must not be represented as final qualification.
