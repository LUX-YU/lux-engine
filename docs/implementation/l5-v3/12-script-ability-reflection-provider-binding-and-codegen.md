# Script Ability Reflection、Provider Binding、Naming 与 Multi-language Codegen

Status: **Normative Implementation Contract — v3 reconciled 2026-09-03**

Implementation status (2026-09-05): **joint closure candidate, awaiting independent review.** Canonical codegen emits the existing
dynamic C++ facade, the coroutine-aware facade and the optional `ScriptAbilityStatic<Ability, Provider>` adapter.
The static adapter is product/composition-local, validates the same ContractId/schema/binding, never enters
ScriptArtifact and does not create a runtime provider registry.

`CppStaticScript` persists explicit coroutine-capable ScriptSymbolIds under Script schema 12 / LXSA wire 10.
Generated Delay ergonomics remain a projection of the owner-side Delay Ability; they are not a second contract.

### Prepared closure and result admission safety

Every Lua Ability/Event closure carries backend identity, an immutable full-userdata prototype-layout token and
an artifact-local ordinal. The bridge validates that token against the active instance before indexing its prepared
span or casting dispatch types. Same-prototype instances use their own prepared provider; foreign-prototype and
stale retained closures fail without provider invocation or waiter registration. Reachable closures keep their
layout token alive, preventing address-reuse ABA. Native-stack execution scopes restore nested calls on success,
failure and yield.

Lua errors/yields must not unwind through C++ noexcept frames. Backend-local Lua wrappers turn typed-thunk status
into Lua error/yield only after the C++ bridge has returned; they do not create another wait token or runtime.

Generic external async results must fit both the runtime payload limit and the 32-byte inline transport, with exact
owned type/layout/alignment. Known unsupported results fail preparation; dynamic factories reject before allocating
an Awaitable/ticket or starting the provider. Owner-local Event/Simulation Delay semantics are a distinct path, not
a thread-detection shortcut. Larger owner-local Event values still use the existing bounded owned representation.

Parent: `11-script-api-capabilities-coroutines-and-await.md`

> 本文件冻结 Script Ability 的 physical ownership、stable identity、code/display naming、receiver/provider binding、CMake opt-in、generated artifacts 与 C++/Lua/FlowForge projection。Project-owned code and source Plugin Packages use the same public contract; there is no separate Plugin Script API registry.

---

## 1. Goal

One canonical declaration must drive every language/tool projection:

```text
Ability declaration with semantic owner
        ↓
canonical generated descriptor/schema
        ↓
generated provider binder/thunks
        ↓
C++ projection
Lua contribution/projection
FlowForge node/catalog projection
future language projections
```

No language/backend owns a second hand-written semantic schema.

---

## 2. Declaration follows semantic owner

Ability declarations live with the package/project that owns the concept.

Examples:

```text
PhysicsQuery3D
    -> Physics domain package

NavigationQuery
    -> Navigation domain package

Delay
    -> Simulation scripting/time owner

InventoryAbility
    -> external project/game package
```

Do not create a central Engine Script SDK package containing every domain contract merely for convenience.

`modules/function/script` owns reusable reflection/codegen/ABI vocabulary, not Engine domain ontology.

---

## 3. Contract identity, source name, display name

These are separate concepts.

```text
ContractId
    stable semantic/runtime identity

code/source name
    stable language/tool-facing identifier

DisplayName
    human/editor presentation text
```

The same distinction applies to methods:

```text
MethodId
method source/code name
method display name
```

Changing display text MUST NOT change:

```text
ContractId
MethodId
Lua source API name
FlowForge semantic node identity
provider resolution
schema identity except where the declaration intentionally changes semantic metadata
```

A display name such as:

```text
"Physics Query 3D"
```

is valid and must not be forced to be a language identifier.

If a declaration does not explicitly provide a code name, the reflected declaration/source identifier may be the deterministic default. Do not invent string heuristics such as removing `Ability` suffixes or converting display text.

---

## 4. Stable contract and method identity

Canonical metadata includes stable typed identities:

```text
ScriptApiContractId
ScriptApiMethodId
schema version/hash
```

The schema must cover the semantic call contract, including at least:

```text
receiver kind
method kind
parameter/result types
pass mode
value lifetime category
```

Names are diagnostics/source/UI metadata, not the hot runtime lookup identity.

---

## 5. Receiver model

v1 receiver kinds:

```text
NONE
PROVIDER_INSTANCE
```

`PROVIDER_INSTANCE` means a generated binder borrows one already-existing runtime provider object.

Generated code MUST NOT:

```text
construct provider
own provider
shared-own provider
perform service discovery
persist provider pointer in ScriptArtifact
```

---

## 6. Provider binder / erased method binding

Owner-side codegen produces typed validation/binding capable of adapting a compatible concrete provider to the canonical Ability description.

Conceptually:

```text
Provider&
    ↓ generated binder
ScriptAbilityBinding
    contract/schema
    owner identity when useful for diagnostics
    receiver/context
    generated method dispatch/thunks
```

Async methods expose a starter/admission thunk rather than pretending to return an immediate eventual result.

The binder only borrows the provider; real lifetime remains with its composition owner.

---

## 7. Method kinds and lifetime metadata

Canonical metadata carries:

```text
QUERY
COMMAND
ASYNC_OPERATION
```

and value lifetime categories:

```text
OWNED_VALUE
STABLE_ID
BORROWED_STEP
AWAITABLE
```

Do not infer these from C++ method spelling.

Examples:

```text
RaycastHit owned result      -> OWNED_VALUE
EntityId                     -> STABLE_ID
component/query temporary    -> BORROWED_STEP
async result                 -> AWAITABLE<...>
```

Detailed cross-await rule is owned by `11`.

---

## 8. Explicit CMake opt-in

Ability reflection/codegen is target/package explicit.

Conceptually:

```cmake
lux_script_abilities(
    TARGET some_target
    SOURCES
        AbilityA.hpp
        AbilityB.hpp
    LOGICAL_PATHS
        ...
)
```

MUST:

```text
explicit source/type inputs
outputs declared to build system
generated files under build/generated or equivalent
incremental dependency tracking
second build no unnecessary regeneration
installed external project support
no source-tree absolute path baked into installed generated closure
```

MUST NOT glob the whole repository for Ability declarations.

---

## 9. Two-stage codegen

### Stage A — canonical owner-side reflection

Produces language-neutral metadata/code:

```text
Ability description
stable IDs
code/display names
schema hash/version
method kinds
value lifetime metadata
typed provider binder/thunks
machine-readable schema/projection data where needed
```

### Stage B — consumer projection

Consumes Stage A:

```text
C++
Lua
FlowForge
future Python/other language
```

Domain package must not depend directly on those language runtime implementations.

---

## 10. ScriptArtifact requirements

Language/tool authoring derives or explicitly selects semantic Ability requirements from canonical Stage-A schema.

Script source/build metadata must not hand-author a schema hash independently.

Requirements store:

```text
ContractId
expected schema hash/version as defined by the ScriptArtifact contract
```

No provider identity.

---

## 11. C++ projection

C++ dynamic/development projection may provide a typed facade over prepared binding:

```text
ScriptAbilityCpp<Ability>
prepared receiver + generated thunk/table
```

S6 may add `co_await` ergonomics for ASYNC_OPERATION.

Future shipping specialization may use known provider/composition information to generate a direct typed path for LTO, but semantic metadata stays identical.

---

## 12. Lua production projection

The old S1.5 proof that directly captured one provider into a Lua VM-global function is not the production authority model.

Production Lua projection is provider-independent at VM registration time.

Conceptual contribution:

```text
ScriptAbilityLuaContribution
    -> canonical Ability description only
```

Backend startup may materialize syntax:

```lua
lux.<AbilityCodeName>.<MethodCodeName>(...)
```

The closure stores only backend/catalog identity sufficient to locate the current executing ScriptInstance's prepared method entry.

Call topology:

```text
Lua language surface
    ↓ small catalog/method ordinal
current executing ScriptInstance
    ↓
per-instance prepared capability entry
    ↓
generated erased method binding
    ↓
composition-owned provider
```

MUST NOT capture a concrete provider as VM-global authority.

An undeclared Ability remains unavailable to a ScriptInstance even if another ScriptInstance in the same VM uses it.

For `BORROWED_STEP`, Lua may receive an owned copied scalar/value only when the bridge can safely copy/marshal at the same step. No raw borrowed pointer exposure.

Lua VM portability requirements are owned by `11`/`07`; Ability codegen must not generate LuaJIT-only canonical semantics.

---

## 13. FlowForge projection

FlowForge Ability nodes are generated/contributed from canonical metadata.

Persistent/semantic node identity uses:

```text
ContractId
MethodId
expected schema
```

Do not persist:

```text
provider class name
Jolt method symbol
raw function pointer
registration ordinal
```

Node pin shape and suspension behavior derive from canonical method kind/value metadata.

Compiler derives ScriptArtifact requirements from actually used Ability nodes, deduplicated by ContractId/schema.

FlowForge async lowering remains an explicit compiler-generated state machine; no FlowForge runtime/scheduler is introduced.

---

## 14. External project support

External projects use the same source of truth and CMake codegen contract.

Example:

```text
MyGame/
  simulation/
    InventoryAbility.hpp
    InventorySystem.cpp
```

Project code can:

```text
declare Ability
reflect/generate canonical metadata
bind its own provider
publish it in Simulation composition
package Lua requiring it
generate FlowForge catalog entries
consume C++ projection
```

No engine repository central registry edit is required.

---

## 15. Source Plugin Package support

A source Plugin Package is only a distribution/build boundary around ordinary targets.

If a package contains a gameplay Ability/System pair:

```text
plugin domain target
    owns declaration + System/Provider
    uses normal lux_script_abilities() codegen
```

If it also contains Editor/Lua/FlowForge support, those are separate higher/consumer targets that consume the canonical metadata.

There is no:

```text
PluginAbilityId
PluginAbilityRegistry
Plugin-owned provider locator
```

Original Ability identities remain canonical.

See `14-plugin-package-and-extension-composition.md`.

---

## 16. Component/ECS projection

Component/meta codegen may feed Script projection, but Component semantics remain with the ECS/Simulation owner.

Dynamic-language `get` must return an owned value or validated step-local representation. It must not make raw ECS storage look indefinitely durable.

Do not create a string query language merely to simplify language binding.

---

## 17. Generated artifact placement / installed SDK

Generated source belongs in build/generated or equivalent project convention.

Installed SDK must expose enough reusable pieces for an external project/plugin source package to:

```text
run Ability reflection/codegen
compile generated binder/projection code
materialize machine-readable Ability schema used by Lua packaging/tooling
consume FlowForge/C++ projection helpers
```

No generated installed consumer may require the original Lux source/build tree.

---

## 18. Diagnostics / fail-closed behavior

Codegen/reflection/projection should fail explicitly for:

```text
invalid/duplicate ContractId or MethodId
invalid code/source identifier
unsupported receiver
invalid method-kind/result combination
unsupported value/pass/lifetime shape
conflicting schema
ambiguous provider publication
language projection cannot represent a required value safely
```

Do not silently omit an Ability method that the artifact/schema claims is available unless the projection contract explicitly marks that method unsupported and causes the consuming asset/build to fail early.

---

## 19. Performance contract

Generated dynamic binding must allow:

```text
composition/mount preparation once
small immutable call-time receiver/thunk path
```

Do not mandate `std::function`, virtual provider inheritance, or per-call reflection/string lookup.

Known shipping specialization remains optional and downstream of the semantic contract.

---

## 20. STOP conditions

STOP for architecture review if implementation appears to require:

```text
central Engine Script SDK containing all domain abilities
codegen constructing/owning providers
provider-specific identity in ScriptArtifact
provider global singleton/service lookup
language-specific semantic schema forks
Lua VM-global concrete provider authority
FlowForge node storing provider identity
runtime string lookup as canonical Script Ability dispatch
source Plugin Package requiring a new Plugin Script registry
language display text used as semantic/runtime identity
```
