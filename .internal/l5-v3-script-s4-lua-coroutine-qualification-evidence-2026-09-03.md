# L5 v3 Script S4 Lua Coroutine Qualification Evidence — 2026-09-03

## Qualified revision

- Production implementation: `b3ddc0af0b6b393d40b925860db961f5f8b8fede`.
- Qualification used the detached worktree `E:/SyncForder/CodeRepos/lux-engine-s4-qualification` at that exact
  revision.
- `git status --short`, `git diff --exit-code`, and `ValidateTrackedSnapshot.cmake` confirmed a clean tracked
  snapshot. The developer checkout's existing `.gitignore` and `WorldPartition.hpp` changes were not included.

## Lua authoring and artifact contract

The production Lua packager now recognizes explicit static metadata:

```lua
---@lux.requires lux.simulation.delay

---@lux.method
---@lux.lifecycle begin_play
---@return void
function EnemyBehavior:initialize()
end

---@lux.method
---@lux.coroutine
---@return void
function EnemyBehavior:update_async()
end
```

`---@lux.lifecycle begin_play|end_play` maps an existing symbol-ledger `ScriptSymbolId` to the lifecycle role;
function names remain diagnostics only. `---@lux.coroutine` marks an exported method as suspension-capable. The
packager rejects duplicate/invalid lifecycle roles, wrong lifecycle signatures, coroutine lifecycle methods, and
non-void coroutine exports. Requirements are explicit and are resolved from generated canonical Ability schema
manifests; Lua source never supplies schema hashes and is not scanned for `lux.X.Y` calls.

`rdesc::Script` schema 8 / LXSA wire 6 hard-cuts `LuaSourceScript.suspension_capable_exports`. There is no old-wire
compatibility shim or lifecycle name fallback.

`lux_script_abilities()` still parses an explicit source list and now emits a language-neutral JSON schema through
a generated C++ schema writer. `lux_package_lua_script()` explicitly declares source, symbol ledger, Ability schema
targets/files, output, module, scope, and semantic value layouts. Generated files stay in the build tree. A second
build reports no work, and an external installed project successfully generated an Inventory Ability, materialized
its canonical schema, packaged Lua source, decoded the LXSA, and constructed the installed Lua runtime backend.

## Prepared Ability topology

The former provider-capturing `projectScriptAbility(lua_State&, binding)` projection was removed. The replacement
`ScriptAbilityLuaContribution` contains only provider-independent canonical metadata.

At backend construction, Lua registers the `lux.<Ability>.<method>` language surface once. Its C closures retain
only the backend state and a small catalog ordinal. At ScriptInstance creation, the backend consumes that mount's
already-resolved `ScriptInstanceCreateContext.capabilities` and builds a bounded dense prepared table containing the
receiver, generated dispatch table, and erased method thunk. A Lua call uses:

```text
current Lua invocation
    -> exact ScriptInstance
    -> prepared method ordinal
    -> generated erased thunk
    -> composition-owned provider
```

There is no VM-global provider authority, per-call ContractId/MethodId string lookup, service lookup, provider
discovery, dynamic cast, or provider ownership. An undeclared Ability remains visible in the VM syntax but fails
closed for that ScriptInstance. Tests use separate backends/providers to prove provider isolation. Synchronous
BORROWED_STEP scalar results are copied into Lua values at the bridge and never expose a C++ pointer.

## LuaJIT coroutine topology

The repository uses LuaJIT `2.1.1771261233` and its Lua 5.1 coroutine ABI. A focused proof established that
`lua_newthread` plus a registry reference survives collection, a C binding may tail-call `lua_yield(L, 0)`, and the
next `lua_resume(thread, nargs)` arguments become the original Lua call's results.

Only exports explicitly listed in `suspension_capable_exports` receive a `BoundScriptStepCall`. Synchronous exports
retain the existing `lua_pcall` path and create no Lua thread, backend continuation, Script continuation, or
awaitable. A suspending invocation owns one bounded backend continuation slot and one Lua registry reference:

```text
long-lived ScriptInstance table
    <- shared object state used by every invocation

invocation-local LuaJIT thread + registry ref
    -> ScriptBackendContinuation
    -> existing ScriptSystem continuation
    -> existing ScriptAwaitable
```

The Lua record never stores ScriptSystem, a provider, or ScriptStepContext. ASYNC_OPERATION creates the engine
awaitable before provider admission and then yields. Eager completion therefore remains tail/stable-point queued.
READY values are validated and copied into Lua resume arguments; FAILED/CANCELLED terminates the invocation without
continuing user Lua. Provider admission failure and raw `coroutine.yield()` without an engine awaitable fail closed.
Sequential awaits reuse the same Lua thread.

Two Event invocations on one ScriptInstance use separate Lua threads while sharing the same instance table. Recurring
Hook single-flight remains entirely owned by ScriptSystem. Retirement destroys/unrefs all gameplay continuation
threads before synchronous EndPlay and instance-table destruction; late completion is stale and cannot call Lua.

Current Simulation tasks and Scene stable points may execute on different OS threads. The Lua backend therefore
serializes access to its one VM internally instead of pinning the VM to its construction thread. No two Lua calls
overlap, external completion never enters Lua, and `lua_resume` executes only in the caller-triggered stable-point
drain. No Lua scheduler or resume queue was added.

## Complexity hardening

PB2 initially exposed an O(N^2) teardown fallback: every `destroyInstance()` scanned the complete Lua continuation
pool. The final revision replaces that scan with exact per-instance continuation accounting. Continuation creation
increments once, every destroy path decrements once, and physical instance destruction enforces the frozen
ScriptSystem invariant that no continuation remains. At 20k instances, EndPlay + destroy dropped from 156–177 ms
in the diagnostic revision to 2.11–2.18 ms in the final revision.

## Correctness qualification

Windows 11 Pro 10.0.26200, MSVC 19.44.35228, x64 RelWithDebInfo, `-j 4 -k 0`:

| Profile | CTest | Second build |
|---|---:|---|
| DEVELOPER | 181/181 | `ninja: no work to do` |
| PLAYER | 181/181 | `ninja: no work to do` |
| EDITOR | 194/194 | `ninja: no work to do` |
| TOOLCHAIN | 168/168 | `ninja: no work to do` |

The final matrix was rerun after the output-sensitive teardown fix. `ValidateSourceArchitecture.cmake` passed in
every clean build. One hundred repeat-until-fail runs passed for:

```text
simulation_script_lua_test
simulation_script_lua_coroutine_integration_test
simulation_script_continuation_test
simulation_script_lifecycle_test
scene_script_runtime_test
scene_script_lua_runtime_test
```

The focused matrix covers prepared QUERY/COMMAND/BORROWED_STEP, delayed and eager async completion, re-suspension,
provider rejection, failed completion, undeclared Ability rejection, raw yield rejection, multi-flight Event
invocations, Hook single-flight, Lua continuation capacity, retirement, late completion, and production Scene
`Delay.nextStep()` ordering across Simulation execution and Scene stable point.

## Installed closure

The clean combined prefix `E:/SyncForder/CodeRepos/install/RelWithDebInfo-s4-qualified` passed
`ValidateInstalledArchitecture.cmake`. Fresh relocated consumers configured and built against the installed prefix:

- Lua Ability authoring/packager/runtime consumer;
- Script Ability codegen consumer;
- Scene Script runtime consumer.

All three executables ran successfully and all three second builds reported no work. The Lua consumer uses an
external Ability declaration and installed CMake helpers without repository-relative generated paths. Changed
public headers were synchronized byte-identically to the Debug, RelWithDebInfo, and Android install include
prefixes. Android was not configured or built.

## Explicitly not done

- S2.4 AssetLoad remains blocked by a script-visible residency-backed Asset handle contract.
- S5 Event.await is not done.
- S5 Physics/Navigation production Abilities are not done.
- S6 C++ coroutine ergonomics is not done.
- S6 shipping static specialization is not done.
- Python runtime is not done.
- Async BeginPlay/EndPlay is not done.
- Temporary Script activation/deactivation is not done.

S4 is qualified. No S5 implementation was started.
