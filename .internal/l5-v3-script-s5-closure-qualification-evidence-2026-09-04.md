# L5 v3 Script S5 closure qualification evidence

> Superseded by `l5-v3-script-s5-final-qualification-evidence-2026-09-04.md`.
> This checkpoint correctly qualified the S5.1 execution mechanisms, but it did not yet persist and validate the
> complete Event contract at mount admission and it predated the canonical Physics2D Simulation provider. It must
> not be used as the final S5 PASS evidence.

Date: 2026-09-04

## Verdict

`S5 BLOCKED`.

S5.0 and S5.1 are qualified. The remaining S5 hard blocker is the absence of an approved ownership and composition
path from the production Physics2D extension into canonical `SimulationSystem` provider publication. No synthetic
Physics provider was substituted.

## Qualified revisions

- Starting revision: `15b83ca7fc2049852aa5491b0f0a6d416618260d`.
- Runtime, language projection, and benchmark implementation: `c9e32413e5c1c19c1d8519df07350f6416523d80`.
- Final consumer qualification fixture closure: `830aa708a7409c52ac003149b718ea0af2007f08`.
- Qualification source: clean detached worktree
  `E:/SyncForder/CodeRepos/lux-engine-s5-qualification` at `830aa708...`.

The only change after `c9e32413...` is an explicit include of the public `NativeModule.hpp` in the installed
FlowForge consumer. It does not change a production target or the measured runtime.

## Canonical Event source projection

`lux::script::ScriptEventSourceDescription` is the reusable, provider-free source description. It contains fixed
system/Event IDs, stable code names, route, payload semantic layout, payload schema ID, and schema version. It owns
no `EventPoint`, provider, handler token, or runtime pointer.

`lux::simulation::script::projectScriptEventSource()` validates and projects a matching `SimulationEventView` and
`ScriptEventEndpointDescriptor`. Contributions are passed explicitly to the FlowForge compiler and Lua backend.
Code names are authoring/prepare-time names only; runtime waits use the already prepared typed IDs in
`ScriptEventWaitRequest`.

## FlowForge Event.await

- `ScriptEventAwaitNode` is a Script graph operation with one execution input, one continuation output, and one
  owned payload output.
- `SCRIPT_EVENT_WAIT` participates in the existing transitive `SuspensionAnalysis`; graph-function calls, lifecycle
  rejection, fan-in any-path analysis, and `BORROWED_STEP` checks share the same suspension truth as async Ability
  nodes.
- AOT lowers the node into the existing explicit invocation state machine. Live owned/stable values and the program
  counter remain in the invocation frame; no native stack is retained.
- Native ABI is hard-cut to version 4 with immutable Event-wait import descriptors and
  `lux_script_step_host.start_event_wait`. The ABI contains fixed scalar vocabulary, not Simulation C++ types.
- `NativeScriptBackend` prepares import ordinals once and maps the host call to
  `ScriptStepContext::event_waits`. Resume payload schema is checked before generated code consumes the owned slot.

There is no FlowForge runtime, waiter store, continuation store, scheduler, or provider lookup.

## Portable Lua Event.await

Lua authoring declares Event sources explicitly before exports:

```lua
---@lux.event Gameplay.damage
```

The runtime surface is:

```lua
local payload = lux.Event.Gameplay.damage()
```

The packager records sorted `(system_name, event_name)` declarations in `LuaSourceScript`. It does not scan Lua call
sites and does not accept numeric IDs. Script schema version is 9 and artifact wire version is 7, with no old/new
compatibility shim.

`LuaScriptBackendConfig` receives explicit canonical Event contributions. VM-global closures contain backend state
and a catalog ordinal only. Each ScriptInstance resolves only its declared sources into a bounded prepared table;
undeclared or missing sources fail closed.

The C binding admits the existing Event waiter and returns. A backend-installed portable Lua wrapper performs
`coroutine.yield()`. This is required because direct yield across the C binding was valid in LuaJIT but unsafe in
PUC Lua 5.4. The user surface, artifact, waiter, continuation, and stable-point semantics remain identical across:

- LuaJIT 2.1.1771261233, JIT enabled;
- LuaJIT 2.1.1771261233, interpreter-only policy;
- Lua 5.4.8.

Resume consumes only `ScriptOwnedResumeValue`. No Event payload pointer, `ScriptStepContext*`, provider pointer, or
VM-specific waiter survives suspension. Synchronous Lua exports remain on the existing `lua_pcall` path.

## Runtime safety

S5.1 reuses the qualified S5.0 `EventBucket`, generational waiter store, `ScriptAwaitable`, `ResumeRing`, per-instance
ownership indices, and `executeStablePoint()` path. Event dispatch may claim a waiter, copy the payload, complete the
awaitable, and enqueue bounded resume work. It never invokes a backend continuation, FlowForge code, or `lua_resume`.

Retirement invalidates the ScriptInstance generation, removes owned waiters/awaitables/continuations, then runs
EndPlay and destroys the backend object. Late Event occurrences and late completion leases fail closed. Hook
single-flight and Event callback multi-flight remain owned by ScriptSystem.

## Domain gates

### Physics: BLOCKED

The only current production Physics owner is `extensions/runtime/physics2d`. It installs through the V5
`luxInstallWorldSystemsV5(ScheduleBuilder&)` extension seam and publishes `PhysicsWorldApi` through the retired typed
World service surface. It is not an installed `lux::simulation::SimulationSystem`.

`SimulationBuilder::publishScriptAbility()` intentionally binds only an already installed SimulationSystem object,
so the existing Physics object cannot honestly be published without changing ownership or adding a prohibited
service locator. The smallest missing architecture decision is to approve a Physics2D migration or narrow Scene
integration that:

1. establishes provider lifetime under canonical Simulation/Scene composition;
2. exposes an installed provider object to `SimulationBuilder`;
3. publishes its generated Ability binding through the existing publication path.

If `PhysicsWorldApi::completedSteps()` remains the first approved operation, it is a synchronous `QUERY`. No raycast,
Jolt/Box2D type, fake provider, or async wrapper was invented.

### Navigation: NOT READY

`Navigation3DBackend::query()` exists, but no canonical Simulation/Scene publication owner exists. Its result also
requires an owned Script collection contract that is not present. No Navigation manager or generic Script array was
added.

### AssetLoad: BLOCKED

The repository still has no residency-backed, cross-language Script asset handle with explicit lifetime/lease
semantics. `shared_ptr`, raw pointers, LuxPak identity, and integerized addresses remain excluded from Script ABI.

## Correctness qualification

Clean Windows RelWithDebInfo matrix at the final qualification source:

| Selected VM | Profile | CTest | Second build |
|---|---|---:|---|
| LuaJIT | DEVELOPER | 185/185 | `ninja: no work to do` |
| LuaJIT | PLAYER | 185/185 | `ninja: no work to do` |
| LuaJIT | EDITOR | 198/198 | `ninja: no work to do` |
| VM-independent | TOOLCHAIN | 169/169 | `ninja: no work to do` |
| Lua 5.4 | DEVELOPER | 185/185 | `ninja: no work to do` |
| Lua 5.4 | PLAYER | 185/185 | `ninja: no work to do` |
| Lua 5.4 | EDITOR | 198/198 | `ninja: no work to do` |

Focused 100-times repeat-until-fail results:

- LuaJIT JIT-on/interpreter, Event waiter, continuation, lifecycle, and Scene Lua runtime: 8/8 tests, 100 repeats.
- Lua 5.4 equivalent suite: 8/8 tests, 100 repeats.
- FlowForge source validation, artifact/AOT, Event state machine, and benchmark smoke: 4/4 tests, 100 repeats.

The focused matrix covers broadcast and self-targeted waits, owned payloads, no-dispatch resume, sequential waits,
re-suspension, eager/nested ordering, fan-in lifetime rejection, lifecycle rejection, capacity failures, retirement,
late occurrence, Hook single-flight, and Event callback multi-flight.

## Architecture and installed closure

The following passed at `830aa708...`:

- `ValidateTrackedSnapshot`;
- `ValidateSourceArchitecture`;
- `ValidateSourceStyle`;
- `ValidateInstalledArchitecture` for independent LuaJIT and Lua 5.4 prefixes.

Installed prefixes:

```text
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s5-luajit
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s5-lua54
```

Fresh relocated consumers for both prefixes configured, built, executed successfully, and produced no work on the
second build:

- `system-event-await-runtime`;
- `flowforge-compiler`;
- `lua-script-packager`;
- `scene-script-runtime`;
- `script-ability-codegen`;
- `system-hook-script-binding`.

The LuaJIT and Lua 5.4 consumers use the same C++, Ability declaration, Lua source, and packaged Event declaration.
No installed generated path refers to the repository source tree.

## Explicit scope

- S5.0 Event waiter runtime: PASS.
- S5.1 FlowForge Event.await: PASS.
- S5.1 Portable Lua Event.await: PASS.
- S5.2 production Physics Ability: BLOCKED by the owner/integration decision above.
- Navigation: NOT READY.
- S2.4 AssetLoad: BLOCKED by the Script asset handle contract.
- Full Physics-inclusive PB3: NOT RECORDED.
- S6 C++ coroutine/static specialization: NOT STARTED.
