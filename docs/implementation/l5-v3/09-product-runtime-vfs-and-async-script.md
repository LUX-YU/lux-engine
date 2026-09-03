# Product Runtime Composition、AssetVfs 与 Cross-time Operation

Status: **Normative Product/VFS Design + historical Script integration context — v3 reconciled 2026-09-03**

> Product/VFS/AssetRead ownership in this document remains normative. Detailed Script Ability/coroutine/Event/Delay semantics are owned by `11`; Script object lifecycle by `13`; implementation order by `07`. Older S0/S1/S2 sequencing previously written here is historical context only.

---

## 1. Product composition objective

Lux final shipping games are project-specific products, not a fixed generic Player architecture.

```text
Project configuration
    + selected runtime/domain modules
    + selected Systems / Scene integrations
    + project code/generated code
    + selected source Plugin Package facets
    + selected Render backend/features
    + cooked/pak content
        ↓
project-specific executable/server/tool target
```

`PLAYER` remains a runtime-clean qualification profile until Product Track P freezes target generation.

Do not invent a universal ProductHost, EngineContext, runtime ServiceRegistry, or fixed Player contract to bypass P.

---

## 2. Product-wide ownership

Shared infrastructure is explicitly owned by product/application composition.

Typical relation:

```text
Application/Product composition owns
    ExecutionRuntime
    mutable AssetVfs control plane
    AssetRead endpoint/port
    RenderRuntime/backend when selected
    Scene/application lifetime roots
```

Consumers receive narrow capabilities/views.

Do not hide these owners behind static singleton facades.

---

## 3. AssetVfs

Preserve:

```text
mutable explicit AssetVfs control plane
copyable/read AssetVfsView
immutable published mount snapshots
provider lifetime retained by snapshots
concurrent reads without UI-owned scanning databases
```

The Editor may mount/unmount project/plugin roots at application composition level and hand `AssetVfsView` to windows.

Do not restore:

```text
AssetVfs::Get()
lazy global mount state
per-window VFS
AssetManager replacement
AssetIndex merely to implement AssetBrowser v1
```

---

## 4. AssetRead / blocking IO isolation

Time-spanning storage reads use the production AssetRead workflow and shared authorized blocking/IO isolation.

```text
stable Asset identity/address
    ↓
AssetVfsView resolution
    ↓
AssetReadPort / typed load operation
    ↓
blocking decode/read stages off owner thread
    ↓
owned completion result
```

A feature must not create its own private thread pool merely because one importer/read blocks.

Do not `sync_wait` a storage Sender on the gameplay/main thread.

---

## 5. General cross-time ownership

The semantic owner of an operation remains the domain that knows the operation.

```text
real-time timer       -> Process execution integration
asset IO/decode       -> AssetRead/asset-loading owner
physics async query   -> Physics/Scene integration
GPU query/readback    -> Render/Scene integration
navigation async work -> Navigation owner/integration
```

L2 execution supplies scheduling/cancellation mechanisms, not domain semantics.

A domain callback must not directly mutate/resume unrelated upper-layer state unless the domain contract explicitly owns that mutation point.

---

## 6. Script suspension relation

When a domain operation is used by Script, Script owns suspension/resume state while the real domain continues owning the work.

```text
Script starts domain operation
    ↓
ScriptSystem creates/owns Awaitable + Continuation relation
    ↓
domain work proceeds
    ↓
domain completion only reports owned result/error/readiness
    ↓
ScriptSystem stable point
    ↓
validate ScriptInstance generation
    ↓
resume backend continuation
```

The domain does not own Lua/FlowForge/C++ continuation representation.

Detailed contract: `11-script-api-capabilities-coroutines-and-await.md`.

---

## 7. AssetLoad current gate

The async mechanism required by Script AssetLoad exists in principle, but AssetLoad is not complete until the script-visible result contract is approved.

The missing decision is not “how to suspend”. It is:

```text
what stable value does Script receive?
what identity does it represent?
what does residency mean?
what lifetime/lease, if any, is guaranteed?
```

MUST NOT expose as canonical Script result:

```text
std::shared_ptr<const ConcreteAsset>
raw asset pointer
void*
uintptr_t
an invented uint64_t handle with unspecified residency semantics
```

Therefore:

```text
S2.4 AssetLoad
    = CONDITIONAL / BLOCKED until Asset handle/value contract
```

S5 may still pass Event/Physics/Navigation closure while this blocker remains explicit.

---

## 8. GPU / Physics cross-frame operations

A render/physics operation that genuinely spans frames may be projected as Script `ASYNC_OPERATION`, but asyncness must come from the domain semantics, not from a desire to demonstrate coroutine support.

Synchronous Physics queries remain synchronous `QUERY` methods.

For GPU work:

```text
Frame N submit
    ↓
GPU/fence/readback owner tracks lifetime
    ↓
readiness later
    ↓
owned completion/adoption
    ↓
Script stable-point resume if a Script is waiting
```

No blocking GPU wait on the normal gameplay path.

---

## 9. Scene / Script cancellation

Script lifetime semantics are defined in `13`, but Product/Scene teardown must preserve these invariants:

```text
stop admitting new Script/domain operations
invalidate ScriptInstance generation before stale completion can apply
cancel/destroy Script continuations under ScriptSystem ownership
clear prepared provider bindings before provider destruction
stop/cancel domain operations where supported
late completion resolves to stale identity and is discarded
```

Do not extend provider/System lifetime with shared ownership merely to avoid fixing teardown order.

---

## 10. Source Plugin Packages and resource/product composition

Plugin v1 is source/build composition, not a runtime plugin manager.

A source Plugin Package may contribute, through ordinary classified targets:

```text
Domain/System/Ability code
RenderFeature code/resources
Scene integration
Toolchain importer/cooker
Editor tools
cooked assets / VFS mount inputs
```

The package does not gain global resource authority. Runtime content is mounted/read through the same product-owned VFS/AssetRead model.

Until Product Track P approves a manifest, CMake target selection is the source-package composition truth. Do not create a second Plugin manifest format.

See `14-plugin-package-and-extension-composition.md`.

---

## 11. Product Track P gate

P must eventually freeze at least:

```text
project identity/configuration
module/System selection
source Plugin Package dependencies/facet selection
native/generated source inputs
static/shared linkage policy
platform/render/backend selection
cooked/pak inputs
host-tool requirements
CMake/generator output contract
```

Before that specification exists, coding agents MUST NOT invent:

```text
Generic Player as final architecture
universal host framework
project manifest syntax
plugin manifest syntax
runtime package/service locator
```

---

## 12. Normative prohibitions

```text
No AssetVfs singleton/static hidden state.
No global EngineContext/service locator.
No synchronous Script-facing disk IO.
No sleep/sync_wait on the game/main thread for cross-time work.
No normal-path GPU blocking wait.
No worker/domain callback directly resuming Script.
No raw/shared concrete Asset pointer as Script AssetLoad result.
No fixed Player as final project-product architecture.
No independent Plugin manifest before Product P.
```
