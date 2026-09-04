# Script API Capability Contracts、Coroutine / Await、Event 与 Portable Backend Contract

Status: **Normative Runtime Scripting Design — v3 reconciled 2026-09-03**

Implementation status (2026-09-04): **S5 PASS; S6 PASS; Script framework frozen.** The existing
ScriptSystem Awaitable/Continuation/ResumeRing remains the only scheduler. C++ coroutine handles and frames are
private to `simulation_script_cpp_static`; Lua threads remain private to the Lua backend; FlowForge continues to
compile into NativeModuleScript.

The first production Physics Ability is the synchronous, backend-neutral `PhysicsQuery2D::overlapsBox` owned by
`engine/domain/simulation/builtin/physics2d`. Box2D remains private. Navigation remains not ready and AssetLoad
remains blocked by the residency-backed Script Asset handle contract; neither reopens the frozen framework.

Parent documents: `00-L5-architecture-overview.md`, `08-normative-execution-contract.md`, `07-implementation-roadmap-and-gates.md`

Companions:

- `12-script-ability-reflection-provider-binding-and-codegen.md`
- `13-script-gameplay-object-lifecycle.md`

> Priority: this document owns Script Ability/capability/provider/coroutine/await/Event/Delay/backend portability semantics. `13` owns gameplay Script object incarnation/lifecycle. `12` owns reflection/codegen/naming/language projection details. General execution/ownership rules from `08` remain in force where not explicitly specialized here.

---

## 1. Unified ontology

```text
Component
    state/data contract

System / integration object
    behavior/runtime owner

Script Ability / Script API
    script-to-engine callable contract

Capability Provider
    existing runtime object implementing an Ability

HookPoint / EventPoint
    engine-to-script execution/event contract

Coroutine / Awaitable
    time-spanning script control-flow contract
```

Key distinction:

```text
System is often a Provider
but Provider is not necessarily a System.
```

Examples:

```text
PhysicsQuery3D       <- Physics System/provider
NavigationQuery      <- Navigation owner/provider
Entity/Component API <- Simulation/ECS integration endpoint
AssetLoading         <- Asset/Scene/Process integration endpoint
Delay                <- Simulation ScriptSystem/time owner
```

---

## 2. `modules/` boundary

Reusable `modules/` may own generic Script vocabulary and ABI/codegen primitives, but Engine ontology stays with the real Engine/domain owner.

Allowed reusable concepts:

```text
ScriptSymbol / ScriptArtifact vocabulary
portable Script value/ABI representation
Ability contract metadata primitives
backend-neutral call/continuation vocabulary
language-generic codegen/projection helpers
Lua/native reusable support
```

Do not push into `modules/function/script` merely to centralize scripting:

```text
Scene
SimulationSystem ownership
SystemInstanceId semantics
Physics/Navigation owner topology
Engine AssetLoading ownership
Scene capability registry
```

External projects may declare their own Abilities in project-owned packages using the same public codegen contract.

---

## 3. Ability contract vs provider implementation

Scripts depend on stable callable semantics, not concrete provider type identity.

```text
PhysicsQuery3D
    raycast(...)
    overlap(...)
        ↓ implemented by
JoltPhysicsSystem or another compatible provider
```

Script source/artifact MUST NOT persist:

```text
JoltPhysicsSystem
provider C++ class name
raw provider pointer
SystemInstanceId as contract identity
registration order
```

Provider binding may keep owner identity for diagnostics/lifetime validation, but Script requirements remain contract/schema based.

---

## 4. Receiver model and ownership

v1 receiver kinds remain intentionally small:

```text
NONE
PROVIDER_INSTANCE
```

Generated binders:

```text
borrow existing provider
prepare immutable dispatch/thunk information
never construct provider
never own/destroy provider
never shared-own provider merely for Script lifetime
```

Real owner creates and destroys the provider.

Teardown invariant:

```text
stop new Script invocations
invalidate ScriptInstances/generations
cancel/destroy continuations
clear prepared capability bindings
then destroy provider objects
```

If this cannot be guaranteed, STOP for ownership review. Do not hide the problem with `shared_ptr<System>`.

---

## 5. Capability publication and ambiguity

Composition publishes bound capabilities before Script mount/admission.

```text
construct/install provider
        ↓
generated bind Ability(provider)
        ↓
publish frozen capability
        ↓
Script mount resolves requirement once
```

v1 default provider rule:

```text
one composition scope
+ one required ContractId
= at most one default active provider
```

Ambiguity fails closed:

```text
SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER
```

Never resolve by first/last registration, string priority, or generic service lookup.

---

## 6. Stable identity / schema

Canonical runtime identity is typed/structured:

```text
ScriptApiContractId
ScriptApiMethodId
schema version/hash
```

Names are for source/codegen/Editor/diagnostics, not the hot-path provider identity.

v1 compatibility is exact schema compatibility. No semantic-version negotiation framework is pre-authorized.

Mount errors must distinguish at least:

```text
SCRIPT_CAPABILITY_NOT_FOUND
SCRIPT_CAPABILITY_SCHEMA_MISMATCH
SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER
SCRIPT_ENDPOINT_NOT_FOUND / invalid endpoint as appropriate
```

---

## 7. Method kinds

```text
QUERY
    synchronous result at a legal semantic point

COMMAND
    synchronous admission/action with no eventual result

ASYNC_OPERATION
    operation may complete later and is projected through ScriptAwaitable
```

Asyncness belongs to canonical metadata. Do not infer it from method names.

A domain operation that is naturally synchronous remains QUERY even if Script supports coroutines.

---

## 8. ScriptArtifact requirements and fail-early mount

ScriptArtifact declares only contracts it actually uses/requires.

```text
Requires:
    lux.simulation.delay / schema X
    physics.query3d      / schema Y
```

Requirement points to semantic contract, never a provider instance/class.

Before Script execution, mount/admission verifies:

```text
artifact/export exists
Hook/Event target exists and is compatible
required Ability contract exists
schema compatible
provider unambiguous
backend executable contract compatible
```

Do not defer a missing Physics/Navigation capability until the first Lua/FlowForge call.

---

## 9. Dispatch strategy is not the contract

Dynamic/development path may use:

```text
prepared receiver/context pointer
+
small ordinal / generated thunk or table
```

MUST:

```text
resolve at composition/mount/instance preparation
immutable prepared binding
no per-call contract/method string search
no dynamic_cast/service lookup hot path
```

Project-specific shipping paths may statically specialize known provider mappings for LTO/devirtualization/inlining, but specialization does not change Ability identity/schema/ownership.

---

## 10. Value lifetime categories

Canonical Ability/schema projection must express at least:

```text
OWNED_VALUE
STABLE_ID
BORROWED_STEP
AWAITABLE
```

Hard rule:

```text
BORROWED_STEP
MUST NOT survive a suspension point.
```

FlowForge statically rejects any path where a borrowed value is live across suspension, including transitive async graph-function calls and fan-in where any path suspends.

Lua/Python must not wrap a borrowed ECS/provider pointer as an apparently durable object.

A synchronous Lua BORROWED_STEP result may be copied into an owned Lua value if the bridge can prove a safe copy/marshal at the same step.

---

## 11. Backend-neutral continuation

User models differ:

```text
FlowForge  sequential graph + compiler-generated explicit state machine
Lua        Lua coroutine/thread
C++        co_await / std::coroutine_handle inside C++ backend
future Python  task/future coroutine
```

Engine model is one backend-neutral continuation contract.

Conceptually:

```cpp
struct ScriptBackendContinuation
{
    void* state;
    ScriptStepResult (*resume)(void*, const ScriptResumePacket&) noexcept;
    void (*destroy)(void*) noexcept;
};
```

`ScriptSystem` MUST NOT know `lua_State`, FlowForge frame layout, or `std::coroutine_handle`.

Explicit invocation results:

```text
COMPLETED
SUSPENDED
FAILED
```

Do not encode SUSPENDED as a magic success/error integer.

---

## 12. Stable generational identity

Cross-time runtime uses stable/generational identities, at least:

```text
ScriptInstanceId
ScriptContinuationId
ScriptAwaitableId
```

Cross-thread completion never carries as lifetime authority:

```text
ScriptInstance*
lua_State*
coroutine_handle*
raw continuation pointer
raw provider lifetime token
```

Completion carries stable identity + owned result/error. Stable-point adoption re-resolves and validates generation.

---

## 13. Awaitable model

All Script-visible time-spanning waits adapt to the same bounded awaitable store:

```text
PENDING
READY
CANCELLED
FAILED
```

Examples:

```text
Delay.nextStep
Delay.seconds / simulationSeconds
Delay.realSeconds
Event next/wait
Asset.load when its value contract is approved
real async Physics/Navigation/GPU operations
```

Storage is bounded + generational.

The eager-completion race is mandatory:

```text
create Awaitable
start provider
provider completes immediately
attach Script continuation afterwards
        ↓
READY waiter tail-enqueued for legal stable-point resume
```

No recursive/inline continuation chain.

---

## 14. Stable resume point

External completion/event may only:

```text
mark Awaitable READY/FAILED/CANCELLED
        ↓
enqueue bounded resume work
```

Script code executes at an explicit Simulation Script stable point.

At resume:

```text
drain up to configured budget
validate ScriptInstance generation
resolve continuation
provide owned result/error
resume backend
```

Never:

```text
worker -> lua_resume
Timer callback -> Script method
physics callback -> FlowForge continue
GPU callback -> coroutine_handle.resume
```

---

## 15. Delay semantics

First version:

```text
Delay.nextStep()
    next eligible Simulation step

Delay.seconds(x)
    == Delay.simulationSeconds(x)

Delay.realSeconds(x)
    monotonic real time
```

Simulation-time delay uses Simulation clock/deadline scheduling. Do not create one process timer per gameplay simulation delay and do not scan every suspended delay every frame.

Principle:

```text
never early
resume at first eligible Script stable point at/after deadline
```

Real-time timer integration reports readiness only; Script still resumes at the Simulation stable point.

---

## 16. Event callback + Event.await

Normal Event callback mode remains valid.

S5 additionally allows a running Script invocation to wait for the next event from the same canonical EventPoint source.

Canonical topology:

```text
EventPoint
    ↓
ScriptSystem EventBucket
    ├─ normal bound handlers
    └─ one-shot coroutine waiters
```

MUST:

```text
waiter bounded + generational
waiter one-shot
normal callback and waiters may coexist
no per-waiter EventPoint.connect/disconnect
dispatch may complete many waiters but never directly executes resumed Script
resume obeys normal stable-point budget
```

Event payload crossing the dispatch lifetime MUST be copied/marshalled into owned resume storage. Never keep the current event call-frame pointer or borrowed payload pointer.

Nested event/retirement correctness:

```text
same waiter cannot complete twice
waiter created during dispatch must not accidentally consume an already-dispatched event
entity/script retirement invalidates waiter before stale resume
nested dispatch ordering must be deterministic under the chosen EventBucket iteration contract
```

Performance invariant:

```text
N idle Event waiters + zero events this frame
must not imply a per-frame O(N) waiter scan.
```

---

## 17. Event fan-out and invocation policy

Recurring Hook invocation remains v1 single-flight:

```text
previous Hook invocation SUSPENDED
    -> next recurring trigger does not start another copy
```

Event invocation may be multi-flight subject to:

```text
per-instance continuation capacity
global continuation capacity
bounded resume queue
per-stable-point resume budget
```

Event.await fan-out may promote many READY awaitables for one Event dispatch. That promotion cost is output-sensitive work and must be measured; resume execution remains budgeted.

---

## 18. Actual async operation ownership

`ScriptSystem` owns:

```text
ScriptInstance runtime state
continuation storage
awaitable storage
resume queue
Event waiter semantics
Simulation-time Delay semantics
```

Real work remains with the real owner:

```text
real timer    -> Process integration
Asset IO      -> AssetRead/asset-loading owner
Physics       -> Physics/Scene integration
Navigation    -> Navigation owner
GPU query     -> Render/Scene integration
```

Simulation scripting support does not justify a private worker pool, VFS blocking IO, GPU fence ownership, or domain manager inside ScriptSystem.

---

## 19. AssetLoad status

The suspension path is not the blocker.

S2.4 remains conditional until Lux freezes a script-visible result contract answering:

```text
stable Asset identity
residency semantics
lifetime/lease semantics if any
cross-language representation
```

Do not expose:

```text
std::shared_ptr<const Asset>
raw Asset*
void*
uintptr_t
unqualified uint64_t pretending to be AssetHandle
```

When the Asset handle/value contract is ready, `Asset.load` should adapt the real AssetRead owner to the existing Awaitable/Continuation path. No new async framework is needed.

---

## 20. Production Physics / Navigation Ability rules

S5 real domain Abilities follow the same contract/provider model.

Ability declaration follows the real domain owner.

Example:

```text
PhysicsQuery3D declaration
    lives with Physics domain

JoltPhysicsSystem
    may provide it
```

Do not put Jolt types into the public Ability signature.

Do not force an operation async merely because Script supports await.

If a result needs an owned collection/path representation and no approved Script value exists, STOP that method or expose a narrower stable semantic value. Do not invent a universal ScriptArray/Variant container for one feature.

---

## 21. Portable Lua backend requirements

Canonical Lua Script artifact remains:

```text
Script::Kind::LUA_SOURCE
UTF-8 source payload
```

No VM-specific canonical bytecode.

Approved portability target:

```text
same Lua source / same ScriptArtifact semantics
        ↓
LuaJIT 2.1 JIT ON
LuaJIT 2.1 JIT OFF
PUC Lua 5.4
```

JIT is an optimization only.

MUST:

```text
VM-specific C API differences isolated in private Lua support
ScriptSystem never knows VM kind
same lifecycle/Ability/coroutine/failure semantics across qualified VMs
portable source profile avoids VM-specific gameplay semantics
portable scalar/value conversion is explicit and lossless
```

Gameplay source must not require as portable semantics:

```text
ffi.*
jit.*
VM-specific bytecode
LuaJIT-only syntax/extensions
Lua-5.4-only source semantics not accepted by the supported LuaJIT parser
```

Do not solve 64-bit identity/value portability through LuaJIT FFI. A stable 64-bit identity should use an approved typed representation rather than an imprecise plain Lua number when exactness matters.

---

## 22. Lua Ability authority

A global Lua language surface such as:

```lua
lux.Delay.nextStep()
lux.PhysicsQuery.raycast(...)
```

may be registered per VM/backend, but the closure must not capture a globally authoritative concrete provider.

Production call topology:

```text
Lua method closure
    -> backend + small canonical/catalog ordinal
    -> current executing ScriptInstance
    -> that instance's prepared capability entry
    -> generated erased thunk
    -> composition-owned provider
```

A Script that did not declare/resolve the Ability cannot use it merely because the VM exposes the syntax and another ScriptInstance has the provider.

---

## 23. C++ coroutine target

S6 may add ergonomic `co_await`, but:

```text
std::coroutine_handle remains C++ backend-private
ScriptBackendContinuation remains Engine boundary
BORROWED_STEP cannot cross co_await
BeginPlay/EndPlay remain synchronous
same Ability metadata remains source of truth
```

Shipping specialization may optimize known provider mappings without changing semantics.

---

## 24. Cancellation / shutdown

Scene stop, Script unmount/reload, entity retirement, and ScriptInstance invalidation must preserve:

```text
invalidate generation first
stop normal dispatch/new invocation admission
cancel/retire Awaitables
request stop on external operation when supported
destroy backend continuations safely
clear prepared capability bindings
only then allow provider destruction
```

Late completion resolving to an old generation is discarded.

Detailed gameplay object ordering: `13`.

---

## 25. Performance / complexity contract

MUST preserve:

```text
ordinary synchronous BoundScriptCall path creates no continuation
sync Ability provider resolution is prepared, not per-call discovered
continuation/awaitable/resume stores are bounded
suspended-idle stable point does not scan every continuation
Delay idle does not scan every pending timer
Event waiter idle does not scan every waiter
object churn follows changed objects rather than total population where the semantic workload is sparse
resume execution obeys configured budget
```

Output-sensitive promotion of K ready deadlines/events is O(K) and may be expensive for extreme bursts; measure before introducing a new scheduling layer.

PB0/PB1/PB2/PB3 exact numbers are evidence, not universal thresholds.

---

## 26. Current implementation sequence

The current Script roadmap is:

```text
S1/S1.5        CLOSED direction
S2.0–S2.3      CLOSED direction
S2.4 AssetLoad CONDITIONAL / blocked by Asset result contract
S2.5 lifecycle CLOSED direction
PB0            recorded
S3             CLOSED direction
S3-H           CLOSED direction
PB1            recorded
S4             CLOSED direction
PB2            recorded
S4-P           portability gate immediately before S5
S5             Event.await + Physics + Navigation when ready + conditional AssetLoad
PB3            gameplay async/domain baseline
S6             C++ coroutine + shipping specialization
then           Script framework FREEZE / R1
```

Exact gate detail is owned by `07`.

---

## 27. STOP conditions

STOP for architecture review if implementation appears to require:

```text
Engine/Scene/System ontology moved into reusable script modules
ScriptApiManager / CoroutineManager / AsyncManager / EventAwaitManager
ServiceRegistry / provider singleton
shared_ptr ownership to keep Systems alive for Script
provider-specific identity persisted in ScriptArtifact
per-call contract/method/provider string lookup
mandatory virtual provider hierarchy as semantic contract
codegen constructing provider objects
borrowed ECS/provider value crossing await
worker/domain callback directly resuming Script
ScriptSystem knowing lua_State / coroutine_handle / FlowForge frame internals
one Lua backend architecture per VM
VM-specific canonical Lua bytecode
AssetLoad returning raw/shared concrete Asset ownership
Physics package depending directly on Lua/FlowForge runtime
```
