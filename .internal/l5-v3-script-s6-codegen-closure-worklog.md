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

## P2 partial checkpoint (2026-09-06)

- Engine code checkpoint: `eca61106e2acb228ed65408f54535d7c503ea71e`.
- Dependency checkpoint: lux-cxx `e2355ca4ce4e862f4e108f74422a30fce8afb736`.
- Native storage populations consume real executable state/frame layouts; public internal page plans are removed.
  Sync modules allocate no continuation frame arena. C++ frame request limits and byte budgets remain explicit;
  private frame-header overhead and generated owned-reference storage are provisioned by the backend.
- Class metadata now accounts only for slots in each page's actual class. Arbitrary empty-page reclassification
  and makeUniformStorageClass (which silently narrowed the promised size) are removed.
- Lua population helpers derive actual entry counts from canonical requirements/contributions. Logical entry
  quotas and byte-budget ceilings remain distinct; tested budgets no longer guess private entry sizeof values.
- Binding-candidate enumeration and explicit/default selection use the existing Script description owner and
  share signature rules with runtime preparation. Basic selection, mismatch, rename-by-symbol and codec tests pass.
  The complete C++/Lua/FlowForge common authoring workflow and source-authored default hints are still incomplete.
- Lua packaging consumes target-generated semantic schema input. The old handwritten scalar size table and
  value/record layout command-line inputs are removed. Generic Event schema output belongs to Script core.
- An attempted new engine/toolchain/script root was rejected by the unchanged architecture guard and removed;
  no replacement aggregate toolchain root or new runtime owner was created.
- P3 handle editing was started prematurely during development regression. It affected the last four negative
  compile tests. The partial change was completely reverted, and that run is excluded from evidence.
  ScriptEventWaitFactory still consumes ScriptEventWaitRequest; P3 is NOT implemented.

### Verified development results

- Fixed P2 snapshot: TOOLCHAIN all + full 199/199 CTest (`p2-stable-tests.log`).
- DEVELOPER all + full 223/223 CTest (`p2-stable-tests.log`).
- Class/storage/native/Lua focused selection: 24/24; later C++/Native/Lua budget regression selection: 20/20.
- Installed consumers: system-hook-script-binding, cpp-coroutine-script, lua-script-packager configure/build/run
  successfully. C++ Event requirement is generated from canonical schema produced by the same composition
  description used at runtime. The first and third second builds report no work; the second needs a final no-op record.
- Current parser requires explicit unexpected<EClassStorageError> in the public class-storage header. That final
  spelling-only correction was validated by the installed coroutine consumer; full profile tests above precede it.
- TrackedSnapshot at eca61106, SourceArchitecture, SourceStyle and InstalledArchitecture for
  install/codegen-p1-developer and install/codegen-toolchain pass.
- Physics's installed schema now participates in all, including TOOLCHAIN with Lua runtime disabled.
- These are incremental development builds, not the requested final independent-clean-clone qualification.

### Overall result: NOT COMPLETE / qualification BLOCKED

The block here is missing implementation/evidence, not a claimed new architecture impossibility:

1. P3 prepared Event handle, one-route claim, owner-only admission, sole payload owner and reentrant copy pin.
2. P4 VM wrapper factory reuse, private typed-helper visibility and Hook reachability caching.
3. Remaining P2 authoring/default/import information accounting and cross-language workflow proof.
4. Clean Debug/Release/Physics-OFF/Lua-disabled/Lua54 qualification, complete relocated consumers, 100x stress.
5. Bsafe/Bwork/Bfinal paired performance, actual assembly/LTO proof, memory ledger and final docs-branch update.

No final production/qualification candidate, optimization speedup, framework freeze, main merge or push is claimed.
