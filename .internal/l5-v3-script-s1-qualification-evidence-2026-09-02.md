# L5 v3 Runtime Scripting S1 Qualification Evidence

Date: 2026-09-02

Status: **PASS — S1 capability and continuation/awaitable foundation is qualified; S1.5 and S2 are not claimed**

Qualified source revision: `bd954001576598ca438b1efd4ef4ab41815b184f`

## Implemented contract

- `ScriptApiContractId` and `ScriptApiMethodId` are stable name identities; method kind is explicit.
- ScriptArtifact schema 6 / wire 4 carries typed contract + schema requirements and rejects older wire.
- ScriptSystem freezes non-owning provider publications, rejects ambiguity, and validates missing/schema mismatch before
  backend instance creation.
- `ScriptInstanceId`, `ScriptContinuationId` and `ScriptAwaitableId` are bounded generational handles.
- Awaitable completion only stores owned result/error and enqueues a stable resume record; backend continuation resumes
  only through `ScriptSystem::executeStablePoint()` with a per-step budget.
- Recurring Hook suspension is single-flight; ready-before-waiter, tail enqueue, cancellation and late completion are
  fail-closed.
- Existing synchronous `BoundScriptCall` remains the default path and allocates no continuation.

## Clean tracked snapshot

An independent detached worktree was created from the qualified revision. It reported an empty porcelain status,
`git diff --exit-code` succeeded, and:

```text
cmake -DLUX_SOURCE_DIR=<clean-worktree> -P cmake/ValidateTrackedSnapshot.cmake
-- Tracked snapshot is clean: bd954001576598ca438b1efd4ef4ab41815b184f
```

The primary worktree's existing `.gitignore` and `WorldPartition.hpp` changes were not included.

## Windows build and test matrix

Every profile used a fresh build tree, RelWithDebInfo, `all -j 4 -k 0`, and CTest inside the MSVC `vcvars64`
environment:

```text
Default Developer: 167/167 CTest passed
PLAYER:            167/167 CTest passed
TOOLCHAIN:         156/156 CTest passed
EDITOR:            180/180 CTest passed
```

All four clean trees received a second full build and reported `ninja: no work to do`.

`simulation_script_continuation_test` passed 100 consecutive repeat-until-fail runs from the clean Developer tree.
Coverage includes sync/no-allocation, suspend/resume/re-suspend, eager ready, typed value/error delivery, resume budget,
tail enqueue, queue/capacity exhaustion, single-flight Hook invocation, cancel/ready race, generation invalidation and
destroy-exactly-once.

## Installed/public closure

- Source architecture validation passed in every profile.
- A fresh combined runtime/editor/toolchain prefix passed `ValidateInstalledArchitecture.cmake`.
- Relocated installed `script_core` consumer configured, linked and exited 0.
- Relocated installed system Hook/Script binding consumer configured, generated metadata, linked and exited 0 while
  exercising a ScriptArtifact requirement and frozen capability publication.
- Both relocated consumer build trees received a no-work second build.
- Changed module public headers were synchronized to Debug, RelWithDebInfo and Android install include prefixes;
  Android configure/build/CTest was not run.

## Explicit boundaries

- S1.5 Ability reflection, receiver metadata, CMake codegen and C++/Lua/FlowForge projection are not implemented.
- S2 NextStep, Delay and AssetLoad are not implemented.
- FlowForge coroutine lowering, Lua coroutine, Event.await, Physics/Navigation abilities, C++ `co_await`, Python,
  provider hot swap and multi-provider routing are not implemented.
- No ScriptApiManager, CoroutineManager, service locator, language-specific common continuation owner or provider shared
  ownership was introduced.

S1 is qualified to serve as the prerequisite for S1.5. This evidence does not authorize starting S2 before S1.5 is
separately implemented and qualified.
