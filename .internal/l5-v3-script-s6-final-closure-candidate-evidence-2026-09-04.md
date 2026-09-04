# L5 v3 Script S6 final closure candidate evidence — 2026-09-04

## Revision and status

- Baseline: `53609e74dc989d2c771c1fd889bfbd720dd688f9`.
- Runtime/test production revision: `d21a36f9669b832ec63ee318ff974e2cd73f5890`.
- Qualification guard revision: `f3178dfc2dd0f38959e16ab2df18c78898d69377`.
- Source: clean `codex/s6-final-performance-closure` worktree.
- Build: Windows 11 x64, MSVC 19.44.35228.0, Ninja, RelWithDebInfo, `-j 4 -k 0`.
- Status: **S6 FINAL CLOSURE CANDIDATE QUALIFIED / AWAITING INDEPENDENT REVIEW**.

The historical freeze evidence for `71842588...` is preserved with a superseded marker. This candidate does not
redeclare the Script framework frozen and does not start R1.

## Correctness and method ownership

`ScriptBackendDescriptor` now prepares one semantic method into one `ScriptBackendPreparedMethod`; synchronous and
resumable views share the same prepared object, and `releaseMethod()` releases it once. The old step-specific
prepare/release pair is removed. CppStatic prepared storage is bounded by explicit actual prepared-method capacity.

Coroutine persistent arguments permit values under the existing declared-value contract. A `const T&` additionally
requires trivially-copyable and trivially-destructible `T`; mutable references, rvalue references, pointers and
non-trivial const references fail projection. `ScriptCoroutineContext` publicly exposes only `ability<>()`, `wait()`
and `delay()`; raw prepared dispatch and `ScriptStepContext` access are private backend/generated-facade details.

ScriptSystem has one logical execution owner for mount, invoke, Event/Delay delivery, stable-point resume and
retirement. A Debug/focused owner-affinity probe permits same-thread nested reentry and fails closed on another
thread. RelWithDebInfo and Release retain no thread-id, atomic or lock probe on that hot path.

## Lua prepared plane

The Lua backend contains no recursive mutex, lock guard, execution vector or reverse scan. Native-stack
`ExecutionFrame`/`ExecutionScope` links nested calls through `active_execution`. Each prototype receives an
artifact-local environment containing only declared Ability/Event surfaces, with the existing portable LuaJIT
`setfenv` and Lua 5.4 `_ENV` seams.

Prepared Ability/Event entries use bounded, reclaimable contiguous spans assigned in deterministic artifact-local
order. Hot calls index `instance.span[local_slot]`; no runtime string lookup, provider discovery, rehash or allocation
occurs. Ability code generation emits method-specific typed Lua entries. Erased bindings remain cold validation and
closed-binary-boundary infrastructure. The 100,000-instance/256-catalog structural fixture prepares two Ability
entries and one Event entry per instance without `instance_capacity * catalog_capacity` storage.

## Awaitable, scheduler and ingress plane

Owner-thread Awaitable/Continuation/Event/Delay/ResumeRing state is lock-free by ownership. NextStep uses a bounded
intrusive generation FIFO with separate current and eligible generations; same-stable-point registration cannot
consume the current generation. Admission is O(1), promotion is O(ready), FIFO ordering is deterministic and no
NextStep heap remains.

Event and Simulation-time Delay completions call the owner completion path directly. External Ability and RealDelay
completions publish compact records into a bounded sequence-number MPSC ring. Producers never mutate owner
AwaitableStorage or ResumeRing. Generational tickets provide exactly-once, FULL/CLOSED/STALE and late-completion
semantics without a global scan. External v1 values are limited to the 32-byte inline owned payload. Eager external
completion is drained only at a legal stable point.

`ScriptOwnedResumeValue` uses compact prepared type metadata on the hot plane. Canonical names, schema and version
remain in artifact/mount validation. Scalar Event, Ability and owner Delay paths have zero measured Engine C++ heap
allocations after preparation. The shared ingress lease remains because its reference counting was below the 5%
profile threshold.

## Native ABI v5 and FlowForge

Native ABI v5 removes the generic `lux_script_ability_runtime::invoke` callback. Generated Native contributions
prepare `{provider_context, dispatch, direct_entry}` for each method. FlowForge-generated LLVM loads the local
prepared entry and calls exact typed QUERY/COMMAND/ASYNC signatures; it no longer constructs erased value-slot
arrays on source-composed Ability calls. ABI v4 fails early with no compatibility branch.

NativeScriptBackend remains FlowForge-independent. FlowForge continues to use the same explicit invocation-local
state machine, Event import ABI, ScriptBackendContinuation, Awaitable and ScriptSystem stable point. Native and C++
coroutine frames remain bounded and record size/alignment/high-water statistics; start, resume/re-suspend and destroy
have no frame heap fallback.

## Shipping specialization proof

The installed `script-ability-ipo` consumer is a PLAYER-style source-composed Release proof, not a new profile. It
uses `check_ipo_supported()`, compiles provider, generated static binding and product composition with `/GL`, and
links with `/LTCG`. Direct, prepared dynamic, generated static and Native typed-entry results are identical. Static
specialization remains an explicit product-side option and is not persisted in ScriptArtifact.

## Correctness and stress coverage

Focused tests cover trivial const-ref acceptance; non-trivial/ref/pointer rejection; one prepared coroutine method at
capacity one; owner affinity; Lua nested reentry and artifact isolation; NextStep FIFO/re-suspend/cancellation;
MPSC multi-producer, FULL, close, eager, double and late completion; FlowForge typed sync/async/Event paths with a
generic-invoke sentinel; ABI v4 rejection; lifecycle, BORROWED_STEP and rematerialization.

- LuaJIT JIT-on: continuation, Event waiter, lifecycle and Lua coroutine integration passed 100 repeats each.
- LuaJIT interpreter-only: VM contract, coroutine integration and Scene runtime passed 100 repeats each.
- Lua 5.4: continuation, Event waiter, lifecycle and Lua coroutine integration passed 100 repeats each.
- Toolchain: FlowForge artifact, Physics FlowForge and runtime integration passed 100 repeats each.

## Build and CTest matrix

| VM | Profile | CTest | final second build |
|---|---|---:|---|
| LuaJIT 2.1.1771261233 | DEVELOPER | 190/190 | no work |
| LuaJIT 2.1.1771261233 | PLAYER | 188/188 | no work |
| LuaJIT 2.1.1771261233 | EDITOR | 203/203 | no work |
| Lua 5.4.8 | DEVELOPER | 188/188 | no work |
| Lua 5.4.8 | PLAYER | 190/190 | no work |
| Lua 5.4.8 | EDITOR | 203/203 | no work |
| VM-independent | TOOLCHAIN | 172/172 | no work |
| LuaJIT, Physics2D OFF | DEVELOPER | 187/187 | no work |

The Physics-OFF compile-command inventory has zero Box2D/Physics2D commands. Debug additionally passed the focused
ScriptSystem owner-affinity test. Android was not configured or built.

## Installed and validator closure

Fresh prefixes:

```text
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s6-final-luajit
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s6-final-lua54
```

Both passed `ValidateInstalledArchitecture`. Against each prefix, these ten consumers fresh-configured, built, ran
and produced `ninja: no work to do` on the second build: `physics2d-script`, `system-event-await-runtime`,
`flowforge-compiler`, `lua-script-packager`, `scene-script-runtime`, `script-ability-codegen`,
`system-hook-script-binding`, `cpp-coroutine-script`, `script-static-ability-specialization` and
`script-ability-ipo`. The same C++ and Lua source is used for both VMs.

The five changed Modules public headers are SHA-256-identical in the Debug, RelWithDebInfo and Android install
include prefixes. `ValidateTrackedSnapshot`, `ValidateSourceArchitecture`, `ValidateSourceStyle` and both installed
architecture validations pass. The installed validator explicitly treats the Lua-specific projection package as the
only legal Simulation scripting surface that can forward-declare `lua_State`; common Script contracts remain
guarded.

## Known debt and verdict

- Lua VM coroutine creation is not pooled because LuaJIT and Lua 5.4 have no common restart/reset contract.
- Lua 5.4 high-volume Event coroutine creation shows VM allocation/GC tail growth; Engine waiter, resume and prepared
  storage remain bounded and allocation-free.
- Hardware-counter sampling was unavailable, so VTune qualification uses user-mode software sampling. Cache/TLB and
  branch-layout tuning are not freeze gates.

No new manager, scheduler, service locator, backend, runtime provider lookup or idle-population scan was introduced.
The candidate is qualified and intentionally awaits independent review before any normative freeze declaration.
