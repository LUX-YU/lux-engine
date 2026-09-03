# L5 v3 Script S3 FlowForge Coroutine Qualification Evidence — 2026-09-03

## Qualified revisions

- Production implementation: `f5f22caae69bdfa31bf1ad7c44fbdd85e2898a06`.
- Benchmark/installed-consumer harness closure: `2ef5d62a6dea4fd7102c47d09ac9ee2e2addde1c`.
- The two revisions after the production implementation change only qualification/benchmark sources. The final
  TOOLCHAIN matrix and focused S3 tests were rerun at `2ef5d62a`.
- `ValidateTrackedSnapshot.cmake` passed from a clean detached worktree at both revisions. The developer checkout's
  pre-existing `.gitignore` and `WorldPartition.hpp` changes were not included.

## Physical topology

| Responsibility | Owner |
|---|---|
| FlowGraph and Script Ability nodes/catalog | `modules/function/flowforge` / `flowforge` |
| Canonical Ability metadata and generated erased thunks | `modules/function/script/core` / `script_core` |
| FlowForge compile, liveness and AOT state-machine lowering | `engine/toolchain/flowforge` / `flowforge_compiler` |
| Generic compiled module ABI/loader | `modules/function/script/{core,native}` |
| Native executable runtime adapter | `engine/domain/simulation/scripting/native` / `simulation_script_native` |
| Awaitable, continuation, resume queue and stable-point owner | `engine/domain/simulation/builtin/script` / `ScriptSystem` |
| Provider ownership | Existing Simulation/application composition owners |

No FlowForge runtime, VM, scheduler, coroutine manager or await manager was added. `NativeScriptBackend` does not
include or link FlowForge, and the generic native ABI contains no Scene, Simulation or ScriptSystem identity.

## Ability node and requirement contract

- `ScriptAbilityNode` owns stable `ContractId`, `MethodId`, expected schema version/hash, method kind and pin metadata.
- QUERY and COMMAND nodes have explicit execution input/output. ASYNC_OPERATION has execution input and a
  continuation output; the awaitable/continuation IDs remain compiler/runtime-hidden.
- `ScriptAbilityNodeCatalog` receives explicit contributions such as
  `makeScriptAbilityCatalogContribution<Ability>()`; it does not use `NodeRegistry::global()` or another singleton.
- The compiler validates the supplied catalog, rejects missing contract/method, schema mismatch and conflicting
  requirements, and derives sorted/deduplicated `ScriptArtifact.api_requirements` only from nodes used by the graph.
- FlowGraph never stores provider type, provider pointer, provider name or `SystemInstanceId`.

## Native ABI v3 and prepared binding

ABI v3 hard-cuts the compiled module contract. It adds:

- method-level immutable `lux_script_ability_import_desc` entries;
- explicit per-instance `lux_script_native_instance_context` containing object state plus a prepared Ability runtime;
- optional `lux_script_step_desc` start/resume/destroy entries per exported function;
- opaque `{slot,generation}` async tokens and owned resume packets.

Generated Ability binding now exposes a narrow erased method table in addition to the existing typed C++ facade.
`NativeScriptBackend` resolves each import once during instance creation and records receiver, typed dispatch and
erased thunk. Call-time dispatch uses a small ordinal. There is no string lookup, provider discovery, service lookup
or dynamic cast per call, and provider pointers are never baked into the FlowGraph, ScriptArtifact or AOT image.

Synchronous-only exports keep the ordinary `BoundScriptCall` path and do not allocate an awaitable, continuation or
invocation frame. Native prepared-call records and continuation-record admission are explicitly bounded by
`NativeScriptBackendConfig`.

## Explicit state-machine lowering

For suspension-capable exports, the AOT compiler:

1. inlines graph functions into the exported invocation and rejects recursive/unsupported async call graphs;
2. lowers structured branch/loop control flow to LLVM CFG;
3. finds async Ability markers keyed by stable node identity;
4. spills invocation arguments, PHIs and live SSA values into a deterministic, alignment-correct frame;
5. assigns program counters in stable NodeId order;
6. emits native start/resume/destroy entries and a switch-based resume core.

The runtime relation is:

```text
module code + immutable import descriptors
    -> ScriptInstance object state + prepared provider receivers
    -> one continuation frame per suspended invocation (pc + live locals)
    -> existing ScriptAwaitable
    -> existing ScriptSystem continuation/resume queue
```

No native stack, fiber, thread or C++ coroutine handle survives a suspension. Multiple invocations receive distinct
frames; frame destruction never destroys long-lived ScriptInstance state. The tested async fixture frame is 40 bytes.

An async call creates the engine awaitable before provider dispatch. Eager completion therefore becomes READY before
waiter attachment and is tail-enqueued for the stable point. Admission failure returns FAILED and releases the frame;
resume failure also returns FAILED. On retirement/shutdown, ScriptSystem invalidates the instance and destroys the
native continuation frame before provider/object teardown; late completion cannot execute generated code.

## Lifetime validation and diagnostics

- BORROWED_STEP values live across an async node are rejected with producer pin and suspension node identity.
- OWNED_VALUE and STABLE_ID values are spillable; resumed eventual values are copied from the owned resume packet
  into a typed frame slot before downstream graph execution.
- Async nodes in BeginPlay or EndPlay exports are rejected. S2.5 lifecycle remains synchronous.
- Deterministic frame size/alignment/hash includes arguments, spill layout, stable suspension identities and semantic
  value identities/lifetimes.
- Tests cover two sequential awaits, re-suspension, eventual-result materialization, branch + loop await, async graph
  function inlining, provider rejection/failure, eager completion, resume budget and retirement.

## Qualification matrix

Clean Windows 11 RelWithDebInfo, MSVC 19.44.35228:

| Profile | CTest | Second build |
|---|---:|---|
| DEVELOPER | 178/178 | `ninja: no work to do` |
| PLAYER | 178/178 | `ninja: no work to do` |
| EDITOR | 191/191 | `ninja: no work to do` |
| TOOLCHAIN | 167/167 | `ninja: no work to do` |

The final `2ef5d62a` TOOLCHAIN rerun was 167/167; the final benchmark-only change does not enter the other profile
closures. `ValidateSourceArchitecture.cmake` passed.

One hundred repeat-until-fail runs passed for:

```text
flowforge_script_artifact_test
flowforge_script_runtime_integration_test
simulation_script_continuation_test
simulation_script_lifecycle_test
simulation_script_native_test
scene_script_runtime_test
```

A fresh combined install prefix passed `ValidateInstalledArchitecture.cmake`. Fresh relocated consumers configured,
built, ran and received no-work second builds:

- `script-ability-codegen` (including generated external Ability -> FlowForge catalog/node contribution);
- `flowforge-model`;
- `flowforge-compiler`;
- `scene-script-runtime`.

Changed public module headers were synchronized to Debug, RelWithDebInfo and Android include prefixes. Android was not
configured or built.

## Explicitly not done

- S2.4 AssetLoad is not done and remains blocked by the script-visible residency-backed Asset handle contract.
- S4 Lua coroutine/yield-resume is not done.
- Lua production lifecycle authoring closure is not done and remains an S4 prerequisite.
- S5 Event.await is not done.
- S5 Physics/Navigation production Abilities are not done.
- S6 C++ coroutine ergonomics is not done.
- S6 shipping static specialization is not done.
- Python runtime is not done.
- Async BeginPlay/EndPlay is not done.
- Temporary Script activation/deactivation is not done.

