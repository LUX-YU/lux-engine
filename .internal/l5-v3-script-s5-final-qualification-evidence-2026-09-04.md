# L5 v3 Script S5 final qualification evidence

Date: 2026-09-04

## Qualified revisions

- Production implementation and complete PB3 benchmark revision:
  `feb19bfb7e3558e8efd85609e83bc143f33a460c`.
- The qualification documentation commits follow that immutable production revision.
- Baseline: `15b83ca7fc2049852aa5491b0f0a6d416618260d`.
- Qualification source: clean detached worktree at
  `E:/SyncForder/CodeRepos/lux-engine-s5-qualification`.

The older `l5-v3-script-s5-closure-qualification-evidence-2026-09-04.md` is superseded. It qualified the Event
execution path but did not yet close Artifact-to-mount Event compatibility and predated production Physics.

## S5.0 and S5.1 Event closure

The S5.0 bounded generational waiter runtime remains unchanged: one endpoint connection per `EventBucket`,
one-shot waiters multiplexed inside `ScriptSystem`, owned payload copy before READY publication, no Event-dispatch
resume, and resume only through the existing bounded `ResumeRing` at `executeStablePoint()`.

S5.1 now has an end-to-end persistent Event contract:

- `rdesc::Script::kSchemaVersion = 10`, ScriptArtifact wire version 8;
- `ScriptArtifact::event_requirements` stores the complete `ScriptEventSourceDescription` actually used by the
  artifact: system/event IDs, route, owned payload canonical name/type/ABI layout, schema hash, and schema version;
- FlowForge derives requirements from actual `ScriptEventAwaitNode` use;
- the Lua packager resolves `---@lux.event System.event` from a canonical generated Event schema manifest and writes
  the complete expected contract, rather than persisting only source names;
- `ScriptSystem` resolves every Event requirement before `createInstance()` and before any BeginPlay/user code;
- mount admission exact-checks endpoint IDs, route, Simulation payload type/name/hash/version, endpoint semantic
  type/pass, owned-copy ABI kind/size/alignment, and copy thunk availability;
- missing endpoints fail with `SCRIPT_EVENT_NOT_FOUND`; every semantic drift fails with
  `SCRIPT_EVENT_SCHEMA_MISMATCH`;
- `NativeScriptBackend` additionally requires every ABI v4 Event import to equal the Artifact requirement;
- `LuaScriptBackend` requires Artifact, mount-prepared source, and explicitly supplied backend contribution to be
  identical before preparing the per-instance ordinal.

Negative qualification covers same source name with changed ID, changed route, changed payload layout, changed
schema hash, changed schema version, and missing source. In every case mount or backend instance creation fails
before the first Script invocation.

FlowForge Event-await continues through its existing explicit state machine and shared suspension/liveness analysis.
Portable Lua continues through the single backend-owned coroutine bridge on LuaJIT JIT-on, LuaJIT interpreter-only,
and Lua 5.4. Neither frontend owns a waiter, scheduler, continuation runtime, or provider authority.

## S5.2 production Physics Ability

The first production Physics Script Ability is owned by the new canonical source extension package:

```text
extensions/physics2d/simulation
    Physics2DSystem (real SimulationSystem and provider)
    Box2DWorld      (private implementation)
    PhysicsQuery2D  (owner-local Ability declaration)
```

`physics2d_simulation` is an installed SIMULATION/RUNTIME/DOMAIN target. It registers through
`SimulationSystemRegistration`, is constructed and owned by `SimulationBuilder`, and publishes its generated
binding through `SimulationBuilder::publishScriptAbility()` only after successful installation. Box2D remains a
PRIVATE implementation dependency. The retired V5 `extensions/runtime/physics2d` World-service installation path
and whitelist entry were removed; no service lookup or adapter registry remains.

The canonical Ability is:

```text
Contract: lux.physics2d.query
Code name: Physics2D
Method: lux.physics2d.query.overlaps_box / overlapsBox
Kind: QUERY
Receiver: PROVIDER_INSTANCE
Result: bool, OWNED_VALUE
Parameters: center x/y and half width/height as owned f64 values
```

`overlapsBox()` is a real gameplay query over the private Box2D world using `b2World_OverlapAABB`; it does not
expose Box2D handles or solver types. The generated binder borrows the already-owned `Physics2DSystem`. C++,
FlowForge AOT/Native, and portable Lua all invoke the same prepared capability and exact provider instance without
per-call provider discovery or Contract/Method string lookup.

## Complete gameplay composition proof

The final Physics benchmark fixture is a single real Simulation + ScriptSystem composition:

- one third long-lived C++ Static entity objects perform normal Hook callbacks and retain object state;
- one third long-lived FlowForge AOT instances call `PhysicsQuery2D.overlapsBox`, await the canonical broadcast
  Event, then await `Delay.nextStep` in the existing generated continuation state machine;
- one third long-lived portable Lua objects execute the identical semantic sequence through the prepared Physics
  Ability, S5.0 waiter, portable coroutine bridge, and built-in Delay provider;
- one broadcast Event occurrence is recorded/drained per frame; dispatch only marks awaitables ready;
- stable-point processing resumes Event waiters, re-suspends on NextStep, and completes them after the next
  Simulation step;
- all runtime storage is pre-sized from the object count and all measured frames report zero C++ heap allocations.

This closes the previously missing Physics-inclusive C++/FlowForge/Lua/Event/Delay PB3 composition without a fake
provider.

## Correctness qualification

All clean Windows RelWithDebInfo configurations built `all` with `-j 4 -k 0`, ran the complete CTest set, and then
produced a no-work second build:

| Lua VM | Profile | CTest | Result |
|---|---|---:|---|
| LuaJIT 2.1 | DEVELOPER | 188/188 | PASS |
| LuaJIT 2.1 | PLAYER | 188/188 | PASS |
| LuaJIT 2.1 | EDITOR | 201/201 | PASS |
| VM-independent | TOOLCHAIN | 171/171 | PASS |
| Lua 5.4.8 | DEVELOPER | 188/188 | PASS |
| Lua 5.4.8 | PLAYER | 188/188 | PASS |
| Lua 5.4.8 | EDITOR | 201/201 | PASS |

LuaJIT's interpreter-only tests are part of every LuaJIT runtime profile. The final focused repeat-until-fail
qualification passed:

- LuaJIT: 8/8 critical Event, continuation, lifecycle, Lua coroutine, Physics integration, and complete mixed
  benchmark smoke tests, each repeated 100 times;
- Lua 5.4: the equivalent 7/7 critical tests, each repeated 100 times;
- TOOLCHAIN: FlowForge Artifact, resumable runtime, Physics integration, and benchmark smoke, 4/4 each repeated
  100 times.

The following validators passed at the clean production revision:

- `ValidateTrackedSnapshot`;
- `ValidateSourceArchitecture`;
- `ValidateSourceStyle`.

## Installed and external closure

Independent installed prefixes passed `ValidateInstalledArchitecture`:

```text
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s5-final-luajit
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s5-final-lua54
```

Fresh relocated builds against both prefixes configured, built, ran, and produced no work on their second build:

- `physics2d-script`;
- `system-event-await-runtime`;
- `flowforge-compiler`;
- `lua-script-packager`;
- `scene-script-runtime`;
- `script-ability-codegen`;
- `system-hook-script-binding`.

The Physics consumer proves installed canonical Ability codegen/schema, real Simulation publication, prepared C++
call, FlowForge compilation, and Lua packaging. The Event consumer now embeds the complete canonical Event
requirement and verifies dispatch followed by stable-point-only resume. No consumer reads the repository source
tree or uses VM-specific Lua source.

## Complexity and architecture result

The S5.0 instrumentation remains the normative complexity proof:

- 10k/50k/100k idle waiters: zero Event waiter visits across stable points;
- 100k global waiters with 10k matching routes: exactly 10k dispatch visits;
- retiring one instance visits only that instance's waiter/awaitable/continuation ownership lists;
- 10k READY with a 2k budget drains in exactly five stable points.

The final Physics/Event/Delay mixed benchmark retains zero measured C++ heap allocations, no per-call provider or
string lookup, and bounded continuation/awaitable/Event/NextStep storage. Source architecture guards confirm that
no Event/Ability/Physics/Coroutine manager, service locator, FlowForge runtime, VM-specific Event backend, global
provider authority, or Engine ontology in reusable Script ABI was introduced.

## S5.3 status and explicit scope

- Navigation: **NOT READY**. A backend query exists, but no approved canonical Simulation/Scene publication owner is
  present and the path result requires an owned Script collection contract that does not yet exist.
- S2.4 AssetLoad: **BLOCKED** by the missing residency-backed, cross-language Script asset handle contract.
- S6 C++ coroutine ergonomics/static specialization: **NOT STARTED**.
- Python runtime: **NOT STARTED**.
- Async BeginPlay/EndPlay and temporary activation/deactivation: **NOT STARTED**.

Navigation and AssetLoad are not S5 PASS blockers under the approved gate. The required production Physics Ability,
Event Artifact compatibility closure, and complete PB3 are now present.

## Verdict

**S5 PASS.** Stop before S6 and wait for independent repository review.
