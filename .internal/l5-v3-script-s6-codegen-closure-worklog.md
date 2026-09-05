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

## P1 development checkpoint

- lux-cxx dependency implementation: `99a5d7e65b25aca1423a98f3d3c8552ca39f6724` (separate branch, not pushed).
  All build and 52/52 dependency CTest; multiline JSON and validation-first rendering are covered.
- Engine-authored `.template` files and `lux_cpp_static_scripts()` now generate constant contract/entry tables
  from real headers plus the existing symbol ledger. One header/target/environment shares one parse job.
- CppStatic no longer uses runtime Ref* projection, typed/reflected matching, reflection invoke or arity tables.
  Native language layout and const-reference ownership are constant-evaluated; objects use typed construction.
- Main C++ tests and benchmarks use generated entries. Generated tables carry no provider or endpoint address.
- A real installed consumer produces a ScriptArtifact, uses the Simulation graph's caller Hook, resumes
  an owned const-reference coroutine through NextStep, retires it and runs EndPlay. Exit 0; second build no-op.
  SDK: `install/codegen-p1-developer`; dependency SDK: `install/script-codegen-cxx`.
  Consumer source: `cmake/installed-consumers/cpp-generated-script`. No Engine source include directory supplied.
- Developer all build + full 209/209 CTest before adding the 13 generation-negative cases.
  The added 13/13 negative cases pass: nontrivial borrowed argument, pointer/mutable/rvalue references,
  non-noexcept method, async lifecycle, missing/duplicate symbol, private method, reason identity,
  borrowed return, missing coroutine context and variadic method.
- Resumable-only prepared entries are valid; lifecycle admission explicitly requires sync-only entries.
  This removes the old need for a dummy synchronous callable on a true coroutine export.
- Isolated dependency includes exposed old-install masking: three codecs used retired ByteIO headers;
  Native fixtures omitted the semantic target; UI test omitted generated headers. Separate commit d5038180
  migrates to current codec error propagation and explicit dependencies. Metadata asset regression passes.
- Installation must use a configured CMAKE_INSTALL_PREFIX: toolset component config destinations are absolute;
  `cmake --install --prefix` alone did not relocate all generated package files. The successful SDK uses
  the configured fresh prefix, not that mixed preliminary install.
- Local logs are `p1-*` in `build/RelWithDebInfo/deep-developer` and `script-codegen-cxx`.
  These are development gates, NOT final clean Bfinal qualification or a performance claim.
- P2 still must migrate remaining installed consumers, close authoring/type-schema inputs and simplify quotas.
  Full artifact/executable compatibility checks still run during instance creation; redundant cold checks and
  fixed local import mapping remain in the P2 information-accounting work. P3/P4 are not implemented.
