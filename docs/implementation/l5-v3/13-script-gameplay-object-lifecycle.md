# Gameplay Script Object Lifecycle、BeginPlay / EndPlay 与 Runtime Incarnation

Status: **Normative Runtime Scripting Lifecycle Contract — 2026-09-03**

Implementation status (2026-09-04): **qualified across C++ Static/coroutine, Native/FlowForge, LuaJIT and Lua
5.4; frozen.** A C++ coroutine invocation frame is invocation-local and bounded. Retirement still destroys all
ordinary backend continuations before synchronous EndPlay and physical object destruction. Coroutine support does
not make BeginPlay or EndPlay asynchronous.

Parents:

- `11-script-api-capabilities-coroutines-and-await.md`
- `08-normative-execution-contract.md`

> 本文件冻结 gameplay Script object 的 incarnation、admission、BeginPlay/EndPlay、retirement、materialization 与 backend object-state contract。它 supersede 任何把 BeginPlay/EndPlay 解释为 scene-global start/stop callback、把 persistent WorldObject identity 等同于 Script object lifetime、或重新引入 retired EntityBehavior lifecycle framework 的旧建议。

---

## 1. Core identity distinction

Lux must distinguish:

```text
Scene lifetime
!= persistent WorldObject identity
!= runtime ECS Entity incarnation
!= ScriptInstance incarnation
```

A persistent object may be unmaterialized and later rematerialized. A runtime Entity may be destroyed/recreated. Each admitted ScriptInstance incarnation has its own gameplay object lifetime.

Therefore:

```text
BeginPlay / EndPlay
belong to one ScriptInstance incarnation,
not to the whole Scene.
```

---

## 2. Canonical object model

One runtime ScriptInstance incarnation maps to one long-lived backend gameplay object/state.

```text
one ScriptInstance incarnation
=
one long-lived gameplay Script object/state
```

Canonical sequence:

```text
create backend object once
initialize context/self/capabilities/prepared methods once
BeginPlay once
normal calls many times
suspend/resume zero or many times
EndPlay once if gameplay lifetime began
destroy backend object once
```

Do not reconstruct the object for every method call.

---

## 3. Physical lifetime vs gameplay lifetime

Physical construction and gameplay admission are separate semantic phases.

A backend object may physically exist while not yet ACTIVE.

Recommended state model:

```text
INACTIVE
CONSTRUCTING
INITIALIZED
ACTIVE
RETIRING
FAULTED
```

Exact enum names may differ, but the semantic distinction must remain.

`createInstance()` is physical construction. `BeginPlay` is gameplay admission. `EndPlay` is gameplay retirement. `destroyInstance()` is physical destruction.

Do not make backend `start/stop` callbacks duplicate BeginPlay/EndPlay.

---

## 4. BeginPlay semantics

BeginPlay means:

> **This ScriptInstance incarnation is fully initialized and is now being admitted into normal gameplay.**

Signature:

```text
void BeginPlay()
```

Properties before BeginPlay:

```text
backend object exists
instance table/member state exists
self/host context valid
required Script capabilities resolved/prepared
normal methods required by the artifact prepared
lifecycle method prepared
```

Normal gameplay Hook/Event binding MUST NOT become visible before successful BeginPlay.

BeginPlay is synchronous in v1.

It MUST NOT suspend/await.

---

## 5. BeginPlay failure

If BeginPlay fails:

```text
instance never becomes ACTIVE
normal gameplay dispatch is never published
no gameplay EndPlay is called for that failed incarnation
physical cleanup still runs
```

The backend object is destroyed according to physical cleanup rules.

Do not call EndPlay as an error-cleanup callback for an incarnation whose gameplay lifetime never began.

---

## 6. Batch admission

When multiple Script objects are materialized/admitted as one stable-point batch, preserve:

```text
create/initialize all candidates
        ↓
all successful candidates INITIALIZED
        ↓
BeginPlay all initialized candidates
        ↓
publish normal gameplay bindings
        ↓
mark ACTIVE
```

The required invariant is:

> **All physical/object initialization for the admission batch completes before the first BeginPlay, and normal gameplay dispatch for the batch is not exposed during BeginPlay.**

This avoids order-dependent half-visible batches.

---

## 7. Runtime spawn

An Entity created after Scene start follows the same local incarnation lifecycle:

```text
Entity/materialization created
    ↓
ScriptInstance constructed
    ↓
dependencies/capabilities/methods ready
    ↓
BeginPlay
    ↓
ACTIVE
```

There is no special “Scene already started so skip BeginPlay” rule.

---

## 8. Streaming / materialization

For a persistent WorldObject that enters the active runtime region:

```text
persistent object already exists conceptually
    ↓
materialize runtime Entity incarnation
    ↓
create new ScriptInstance incarnation
    ↓
BeginPlay
```

On unmaterialization:

```text
ScriptInstance RETIRING
    ↓
EndPlay(OBJECT_UNMATERIALIZED or approved equivalent)
    ↓
destroy backend object
    ↓
remove runtime Entity incarnation as ordered by the owner
```

If the persistent object materializes again later:

```text
new runtime Entity incarnation
new ScriptInstance generation/object
new BeginPlay
```

Runtime Script fields are incarnation-local/transient unless a separate persistent gameplay data owner explicitly stores them.

---

## 9. EndPlay semantics

EndPlay means:

> **This ScriptInstance incarnation is permanently leaving normal gameplay.**

Signature:

```text
void EndPlay(ScriptEndPlayReason reason)
```

`ScriptEndPlayReason` is a real semantic type owned by the Simulation scripting/lifecycle owner. Do not replace it with an unqualified integer merely because its ABI representation is integral.

EndPlay is synchronous in v1.

It MUST NOT suspend/await.

---

## 10. Retirement ordering

Before EndPlay:

```text
mark RETIRING
stop new normal Hook/Event dispatch to the incarnation
remove/invalidate normal gameplay bindings
invalidate ScriptInstance generation for future stale resolution
cancel/retire ordinary Awaitables
remove/destroy backend gameplay continuations at a safe point
```

Then:

```text
EndPlay(reason)
```

Then:

```text
release lifecycle/normal/step prepared methods
destroy backend object
clear remaining instance/backend state
```

The key guarantee is:

> No gameplay continuation belonging to the retiring incarnation may resume after EndPlay begins.

---

## 11. EndPlay failure

If EndPlay itself reports/faults:

```text
diagnostic/failure is recorded
physical destruction continues
```

Do not leave the object or provider bindings alive merely because EndPlay failed.

---

## 12. EndPlay reasons

Reasons must correspond to paths the runtime can actually prove.

Approved examples when real paths exist:

```text
ENTITY_DESTROYED
OBJECT_UNMATERIALIZED
RUNTIME_STOPPED
FAULTED
```

Additional reasons such as `MOUNT_REMOVED` or `SCRIPT_REPLACED` may be added only when those distinct retirement paths actually exist and can be observed reliably.

Do not add speculative reason values merely for enum completeness.

---

## 13. Entity destruction caveat

`EndPlay(ENTITY_DESTROYED)` does not imply arbitrary ECS components remain available.

The Entity owner may already be in destruction semantics. Script lifecycle guarantees Script object/context ordering, not preservation of every component for teardown callbacks.

If a specific gameplay contract requires data during EndPlay, that data must have a real lifetime owner/contract.

---

## 14. Continuation retirement / late completion

Canonical safety sequence:

```text
ACTIVE Script invocation
    ↓
suspends on Awaitable
    ↓
Entity/object/runtime retires
    ↓
ScriptInstance generation invalidated
    ↓
backend continuation destroyed
    ↓
EndPlay
    ↓
backend object destroyed
```

If the external operation completes afterwards:

```text
completion resolves old generation
    ↓
INVALID / stale
    ↓
no resume
no backend code execution
no provider access
```

This rule is language-independent.

---

## 15. Multiple invocation relation

One ScriptInstance object state may have zero or many invocation continuations.

```text
ScriptInstance object state
    shared by normal methods and invocations

Invocation continuation A
    invocation-local stack/state

Invocation continuation B
    independent invocation-local stack/state
```

Event invocations may be multi-flight. Hook single-flight policy is owned by ScriptSystem.

Destroying one continuation does not destroy the Script object. Retiring the Script object destroys all of its continuations before EndPlay.

---

## 16. Backend mappings

### C++ Static

```text
one real reflected/native object per ScriptInstance
member state persists across calls
BeginPlay/normal calls/EndPlay see the same object
physical destructor exactly once
```

### Lua

```text
one independent instance table per ScriptInstance
Lua coroutine/thread is invocation-local
multiple coroutines may share the same instance table
EndPlay executes synchronously on the still-live instance table after gameplay continuations are gone
```

### FlowForge / compiled native

```text
one long-lived generated instance state per ScriptInstance
compiler-generated continuation frame is invocation-local
continuation destruction never destroys instance state
```

Every backend maps to the same Engine lifecycle semantics.

---

## 17. Lifecycle authoring metadata

Lifecycle roles are explicit artifact metadata pointing to stable `ScriptSymbolId` values.

Do not infer lifecycle from names such as:

```text
BeginPlay
EndPlay
OnStart
OnStop
```

A language/tool may use explicit annotations/options to assign an exported symbol to a lifecycle role, but the final ScriptArtifact stores stable symbol identity.

Lifecycle role target must be an exported function with the exact required signature.

---

## 18. FlowForge lifecycle rule

FlowForge lifecycle export must be statically proven non-suspending.

This includes transitive graph-function calls.

```text
BeginPlay -> GraphFuncCall -> async Ability
```

is illegal even when the lifecycle entry does not directly contain the async node.

---

## 19. Lua lifecycle rule

Lua packager authoring uses explicit lifecycle metadata mapped through the normal symbol ledger.

Lifecycle function name is diagnostic/source syntax only.

A lifecycle export cannot also be marked coroutine-capable.

If dynamic Lua code attempts raw yield/async suspension from BeginPlay/EndPlay despite static metadata, the invocation fails closed; gameplay/physical cleanup ordering still follows this document.

---

## 20. Temporary activation / disable is not lifecycle

Do not overload BeginPlay/EndPlay for:

```text
LOD throttling
AI sleep
visibility disable
temporary script pause
editor preview mute
streaming prefetch state while still materialized
```

Those are activity/scheduling policies.

If a future explicit activation/deactivation feature is needed, design it separately from object incarnation lifetime.

---

## 21. Scene stop

Runtime/Scene shutdown retires active Script incarnations through the same ordering:

```text
stop normal admission/dispatch
retire continuations/awaitables
EndPlay(RUNTIME_STOPPED)
destroy backend objects
clear prepared provider bindings before provider owner teardown
```

Do not special-case shutdown by dropping backend objects without lifecycle retirement unless the process is already in an unrecoverable abort path where normal semantics cannot run.

---

## 22. Performance / complexity

Lifecycle operations should be output-sensitive to the changed objects.

Sparse churn:

```text
large stable population
small number retire/rematerialize this frame
```

must not force a full-population scan solely for Script lifecycle bookkeeping when the owner already identifies changed instances.

Continuation teardown for one instance must not scan the entire global continuation pool if exact per-instance accounting/ownership can be maintained.

---

## 23. Qualification focus

At minimum prove:

```text
BeginPlay once per successful incarnation
EndPlay once per successfully begun gameplay lifetime
failed BeginPlay never ACTIVE and receives no EndPlay
same backend object state across lifecycle + normal calls
batch initialization precedes first BeginPlay
normal dispatch not visible during BeginPlay
runtime-created entity receives BeginPlay
unmaterialization receives EndPlay
rematerialization creates new object/generation/BeginPlay
suspended invocation retired before EndPlay
late completion cannot resume old incarnation
shutdown reason/order correct
C++ Static / Lua / Native/FlowForge backend behavior consistent
```

---

## 24. STOP conditions

STOP if implementation appears to require:

```text
EntityBehavior resurrection
ScriptComponent authored lifecycle state resurrection
backend startInstance/stopInstance lifecycle duplication
lifecycle inferred from method names
async BeginPlay / async EndPlay
LifecycleManager/Coordinator merely to forward ScriptSystem state
persistent WorldObject identity reused as ScriptInstance generation
ordinary continuation surviving into EndPlay
shared backend object for unrelated ScriptInstances
```
