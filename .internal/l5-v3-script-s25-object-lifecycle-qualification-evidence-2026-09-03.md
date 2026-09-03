# L5 v3 Runtime Scripting S2.5 Object-Lifecycle Qualification Evidence

Date: 2026-09-03

Status: **PASS — gameplay ScriptInstance incarnation lifecycle is qualified; PB0 and S3 are not claimed**

Qualified production revision: `10b68dcf1258615538e1a2519e50519c52083ff8`

## Lifecycle contract

- `rdesc::Script` schema v7 and ScriptArtifact wire v5 carry optional `begin_play` and `end_play`
  `ScriptSymbolId` roles. Export names remain diagnostic only; there is no lifecycle name inference.
- Physical construction, gameplay admission, gameplay retirement and physical destruction are distinct:
  `createInstance -> INITIALIZED -> BeginPlay -> ACTIVE -> RETIRING -> EndPlay -> destroyInstance`.
- BeginPlay is an optional synchronous `void()` export. All mounts in one materialization batch finish resolve,
  capability binding, backend construction and method preparation before the first BeginPlay runs. Normal Hook/Event
  bindings are published only after every successful BeginPlay in the batch.
- EndPlay is an optional synchronous `void(ScriptEndPlayReason)` export. Retirement first marks every member of the
  dirty batch unavailable, removes normal dispatch, invalidates its generation, cancels awaitables and destroys
  continuations; only then does the batch invoke EndPlay and release methods/backend objects.
- `ENTITY_DESTROYED`, `OBJECT_UNMATERIALIZED`, `RUNTIME_STOPPED` and `FAULTED` are stable Simulation Script-owned
  reasons. EndPlay failure is diagnostic and never prevents physical destruction. BeginPlay failure never publishes
  ACTIVE and does not invoke EndPlay.
- A rematerialized persistent WorldObject receives a new backend object and ScriptInstance generation. BeginPlay and
  EndPlay are exactly once per successful runtime incarnation, not once per Scene or persistent identity.
- A late completion retained past entity destruction observes an invalid awaitable generation and cannot resume or
  access the retired object.

## Backend coverage

- C++ Static constructs a real reflected object per instance. A custom-named lifecycle pair mutates member state;
  a later normal call observes the same state; EndPlay observes the final state and typed reason; the destructor runs
  exactly once.
- Lua uses two independent instance tables from one source prototype. Explicit lifecycle exports operate on each
  table's own state, and the existing normal-call independence checks remain intact.
- Native reuses one allocated instance state across lifecycle and normal prepared calls. Generic native/Lua ABI
  validation now accepts explicitly named custom scalar semantics such as the typed EndPlay reason without treating
  them as built-in identities.
- FlowForge compilation copies explicit lifecycle roles into the same canonical ScriptArtifact. No FlowForge
  coroutine/runtime owner was added.
- The Lua static packager was hard-cut to schema v7/wire v5 and emits empty lifecycle roles when none are explicitly
  authored; no compatibility decoder or method-name fallback was introduced.

## Clean tracked snapshot

An independent detached worktree at the qualified revision had empty porcelain status, passed `git diff --exit-code`,
and passed:

```text
cmake -DLUX_SOURCE_DIR=<clean-worktree> -P cmake/ValidateTrackedSnapshot.cmake
-- Tracked snapshot is clean: 10b68dcf1258615538e1a2519e50519c52083ff8
```

The primary worktree's existing `.gitignore` and `WorldPartition.hpp` changes were excluded. Bellman experiment
processes and files were not touched.

## Windows RelWithDebInfo qualification

Each profile used its own build tree, `all -j 4 -k 0`, full CTest and a second complete build:

```text
Default Developer: 176/176 CTest passed
PLAYER:            176/176 CTest passed
TOOLCHAIN:         163/163 CTest passed
EDITOR:            189/189 CTest passed
Second build:      ninja: no work to do (all four profiles)
```

The clean Developer tree passed 100 consecutive repeat-until-fail runs for each of:

```text
simulation_script_lifecycle_test
simulation_script_continuation_test
scene_script_runtime_test
```

The Toolchain matrix caught and closed the stale Lua packager schema before this revision was qualified.

## Installed and external closure

- Fresh Developer/Toolchain/Editor installs composed into a new prefix and passed
  `ValidateInstalledArchitecture.cmake`.
- `ValidateSourceArchitecture.cmake` passed.
- Fresh relocated consumers configured, built, executed and received no-work second builds:
  - `scene-script-runtime`
  - `simulation-composition`
  - `system-hook-script-binding`
  - `script-ability-codegen`
- Installed codegen/package imports did not require the repository source tree.
- Android configure/build/CTest was intentionally not run; this checkpoint qualifies Windows RelWithDebInfo.

## Explicitly not done

- S2.4 AssetLoad remains blocked by the missing script-visible residency-backed Asset handle contract.
- S3 FlowForge coroutine lowering is **not implemented**.
- S4 Lua coroutine/yield-resume is **not implemented**.
- S5 Event.await and production Physics/Navigation Abilities are **not implemented**.
- S6 C++ coroutine ergonomics/static specialization is **not implemented**.
- Python runtime is **not implemented**.
- Temporary Script activation/deactivation lifecycle is **not implemented**.
- Async BeginPlay and async EndPlay are **not implemented**.

No EntityBehavior framework, lifecycle annotation family, backend start/stop callback, lifecycle manager, Scene/World
streaming dependency, provider shared ownership, service locator or name-based lifecycle inference was introduced.
