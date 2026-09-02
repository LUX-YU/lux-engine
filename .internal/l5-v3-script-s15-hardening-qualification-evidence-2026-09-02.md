# L5 v3 Runtime Scripting S1.5-H Qualification Evidence

Date: 2026-09-02

Status: **PASS — Script Ability/continuation hardening and S2 preflight are qualified; S2.0 and later scripting
waves are not claimed**

Qualified source revision: `d877bce673882aa996f5652de8e88a3a1f72ca7f`

## Closed contract gaps

- `ASYNC_OPERATION` is no longer exposed as an immediate-result method by the generated C++ facade. Generated
  provider dispatch instead accepts a hidden typed completion and returns an explicit operation-admission result.
- `invokeScriptAbilityAsync()` creates the existing ScriptSystem awaitable before invoking the provider, adapts the
  generic Ability completion to `ScriptAwaitableCompletion`, and returns `SUSPENDED` only after successful provider
  admission. Admission failure discards the awaitable and fails closed.
- Eager, delayed, failed, double and late completion all use the existing bounded awaitable ingress/resume queue.
  Completion never resumes script execution directly; only an explicit `executeStablePoint()` drains resumes.
- `ScriptRuntimeLimits` now has an independently enforced per-instance continuation capacity. The global pool remains
  a separate bound, and every completion, failure, rollback, cancellation, unmount and shutdown path releases
  accounting exactly once.
- Ability compatibility hashes traverse methods in canonical full `ScriptApiMethodId` order. Source/display order is
  retained in the descriptor but does not affect compatibility. Duplicate method IDs are rejected.
- Ability codegen accepts values and `const T&` only. Mutable lvalue references, rvalue references and unsupported
  volatile reference shapes are rejected instead of being misdescribed as `CONST_REF`.

## Performance and lifetime contract

- Existing synchronous `BoundScriptCall` execution is unchanged and allocates no continuation.
- QUERY/COMMAND prepared dispatch does not use the async bridge. No per-call provider discovery, dynamic cast,
  service lookup or runtime string lookup was added.
- An actual async invocation consumes one bounded Script awaitable and, when it suspends, one bounded continuation.
  Its copyable completion owns only an adapter/awaitable-ingress lease; it owns neither ScriptSystem nor provider.
- Simulation continues to own provider Systems. Generated bindings borrow the exact already-created provider.
  Shutdown invalidates Script instances and awaitables before capability use-sites/provider destruction, so late
  completion is rejected without gameplay execution.

## Clean tracked snapshot

An independent detached worktree was created from the qualified revision. It reported an empty porcelain status,
`git diff --exit-code` succeeded, and:

```text
cmake -DLUX_SOURCE_DIR=<clean-worktree> -P cmake/ValidateTrackedSnapshot.cmake
-- Tracked snapshot is clean: d877bce673882aa996f5652de8e88a3a1f72ca7f
```

The primary worktree's existing `.gitignore` and `WorldPartition.hpp` changes were not included. Bellman experiment
processes and files were not touched.

## Windows RelWithDebInfo build and test matrix

Every profile used a fresh build tree, MSVC RelWithDebInfo, `all -j 4 -k 0`, and CTest in the `vcvars64` environment:

```text
Default Developer: 174/174 CTest passed
PLAYER:            174/174 CTest passed
TOOLCHAIN:         161/161 CTest passed
EDITOR:            187/187 CTest passed
```

All four profile trees then reported `ninja: no work to do` on a second `all -j 4 -k 0` build.

The clean Developer tree also passed 100 consecutive repeat-until-fail runs for each of:

```text
simulation_script_continuation_test
simulation_script_ability_codegen_test
simulation_script_ability_projection_test
```

The full profile tests cover delayed/eager/failed/admission-rejected/double/late/cross-thread Ability completion,
per-instance and global continuation bounds, quota return, canonical hash invariants, duplicate MethodId rejection,
and mutable parameter/result rejection.

## Installed and external closure

- A fresh combined Developer/Toolchain/Editor prefix passed `ValidateInstalledArchitecture.cmake`.
- `ValidateSourceArchitecture.cmake` passed and now rejects Simulation ontology or manager/service vocabulary in the
  generic module async completion surface.
- A fresh relocated `script-ability-codegen` consumer loaded the installed helper, generated an async Inventory
  Ability, compiled its provider completion/starter proof, ran successfully, and received a no-work second build.
- Fresh relocated `simulation-composition` and `system-hook-script-binding` consumers configured, linked, ran
  successfully, and each received a no-work second build.
- The installed codegen path used installed assets and did not depend on a repository-relative source path.
- Android configure/build/CTest was intentionally not run; this checkpoint qualifies the requested Windows
  RelWithDebInfo profiles only.

## Explicitly not done

- S2.0 Production Script stable-point integration is **not implemented**.
- S2.1 NextStep is **not implemented**.
- S2.2 Simulation Delay is **not implemented**.
- S2.3 Real Delay is **not implemented**.
- S2.4 AssetLoad is **not implemented**.
- S3 FlowForge coroutine lowering is **not implemented**.
- S4 Lua coroutine is **not implemented**.
- S5 Event.await and production Physics/Navigation Abilities are **not implemented**.
- S6 C++ coroutine ergonomics/static specialization is **not implemented**.
- Python and multi-provider routing are **not implemented**.

No Script/Ability/Coroutine manager, service locator, provider shared ownership, runtime string dispatch, second
continuation runtime, mutable-reference ABI, language coroutine backend, provider hot swap or multi-provider routing
was introduced.

S1.5-H is qualified enough to enter S2.0. This evidence explicitly stops before S2.0 implementation.
