# Scene Runtime World IO / Process / Materialization 实施规范（v3.1）

> 状态：Normative。
>
> 施工 Phase：**8**。
>
> 前置：World Phase 2–3、ECS Phase 4、Simulation Phase 5、Scene Phase 7 已完成。
>
> 目标：只实现 **World 数据读取与 Registry materialization mechanism**，不实现 streaming policy/ownership framework。

---

## 1. Public Type Budget

本 Phase 只允许新增：

```text
ReadWorldStorageRange
WorldStorageSource
WorldMaterializer
WorldStorageRuntimeFailure
WorldMaterializeFailure
```

`loadWorldPartition(...)` 是 free function / Sender-producing function，不创建 owner class。

禁止新增：

```text
WorldLoader
WorldPartitionLoader
SceneStreamingRuntime
WorldStreamingRuntime
WorldMaterializationPlan
WorldMaterializationRegistry
WorldMaterializationBinding
WorldMaterializationScratch
WorldMaterializationContext
PartitionResidency
WorldRuntimeContext
```

---

## 2. Runtime package ownership

Physical leaf 固定：

```text
engine/scene/runtime/world
```

Layer：

```text
LAYER SCENE
```

它可以依赖：

```text
WORLD
SIMULATION ECS core/schema
PROCESS OperationPort/Sender + byte IO capability
CORE/RESOURCE minimal capabilities
```

它不得依赖：

```text
scene/spatial3d
scene/spatial2d
Render backend
Editor
Authoring
Toolchain
Host main loop
```

---

## 3. WorldStorageSource = bundle-bound value capability

异步 lifetime 的核心规则：operation 启动后不能借用 Scene memory。

因此 `WorldStorageSource` 必须自己 retain：

```text
WorldDescription shared lifetime
bundle origin/provider state shared lifetime
read capability/port lifetime
```

Repository erratum：原始 v3.1 的 `Impl` 不需要。`OperationPort<T>` 已通过
`shared_ptr<Endpoint>` 持有 provider-specific lifetime；physical origin、volume→handle mapping、
Pak/VFS/network/File IO capability 全部由 private Endpoint 持有。

Public shape 冻结为：

```cpp
class WorldStorageSource final
{
public:
    WorldStorageSource() noexcept = default;

    [[nodiscard]] static
    lux::cxx::expected<WorldStorageSource, WorldStorageRuntimeFailure>
    create(
        std::shared_ptr<const world::WorldDescription> world,
        lux::async::OperationPort<ReadWorldStorageRange> read_port
    ) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;

    [[nodiscard]] const world::WorldDescription&
    world() const noexcept;

    [[nodiscard]] const lux::async::OperationPort<ReadWorldStorageRange>&
    readPort() const noexcept;

private:
    std::shared_ptr<const world::WorldDescription> world_;
    lux::async::OperationPort<ReadWorldStorageRange> read_port_;
};
```

`create()` 只验证：

```text
world != nullptr
read_port valid
```

失败统一返回 `WorldStorageRuntimeFailure::INVALID_SOURCE`。它不打开文件、不解析 path、
不读取 sidecar，也不复制 provider state。

不要为 provider 再创建：

```text
WorldStorageProviderAdapter
WorldBundleResolver
WorldStorageManager
```

Product/provider integration 先建立 private `OperationPort<ReadWorldStorageRange>::Endpoint`，
再调用 `WorldStorageSource::create(world, port)`。

---

## 4. ReadWorldStorageRange operation

冻结 semantic operation：

```cpp
struct ReadWorldStorageRange final
{
    using Value = SharedBytes<>;
    using Error = WorldStorageRuntimeFailure;

    std::uint32_t volume{};
    std::uint64_t offset{};
    std::uint64_t size{};
};
```

实际 `SharedBytes<>` 类型名按 repo 现有 byte owner复用。

重要：

```text
volume = WorldDescription volume ordinal
```

不是 filename/path。

Process 只看到 operation/value/error，不理解 World semantic。

---

## 5. Source construction and bundle verification

`WorldStorageSource` 必须与一个已经 decode/validate 的 WorldDescription 成对建立。

构建 Source 时至少确认：

```text
bundle physical origin can address every declared volume member
member naming/path policy valid
root WorldDescription is retained
```

真正打开每个 sidecar 时，依 `02` 再校验：

```text
BundleId
Generation
VolumeOrdinal
format/file bounds
```

禁止：

```text
AssetVfs::pathOf(root) + string sibling guess
```

---

## 6. loadWorldPartition exact seam

冻结调用形状：

```cpp
[[nodiscard]] auto loadWorldPartition(
    WorldStorageSource source,
    world::WorldPartitionOrdinal partition,
    std::stop_token stop
);
```

返回值必须满足 stdexec Sender contract，成功 value：

```text
world::WorldPartitionData
```

operation state 必须 own：

```text
WorldStorageSource by value
partition ordinal
stop token/state
all temporary decoded buffers required until completion
```

operation state **不得持有**：

```text
Scene&
Registry&
System&
WorldDescription& borrowed from Scene
WorldMaterializer& borrowed from Scene
```

---

## 7. Partition load workflow

Frozen workflow：

```text
partition ordinal
    -> WorldDescription partition-table page directory
    -> read table page chunk descriptor/payload
    -> decode table page
    -> find partition record/extents
    -> for each extent:
         read chunk descriptor(s)
         exact payload range read
         digest verify
         decompress/decode
    -> assemble one WorldPartitionData
    -> set_value
```

v1 multi-extent can be serial。

No parallel fanout framework needed。

如果 benchmark 证明 fanout必要，另设计；本 Phase 不创建 generic concurrency coordinator。

---

## 8. Page cache

Phase 8 **不要求新增 public cache type**。

如果 partition-table page重复读取成为明显成本，可在 `WorldStorageSource::Impl` 内做 bounded/private cache。

禁止 public：

```text
WorldPartitionPageCache
WorldCacheManager
```

除非以后出现第二个 independent consumer。

Private cache必须：

```text
bounded by explicit internal config/constant
threading matches Source execution use
no gameplay policy
```

第一版也允许完全无 cache 先通过 correctness/perf probe。

---

## 9. No object-level IO

`WorldPartitionData` 到内存后，object-level selection是 CPU-only。

禁止实现：

```cpp
loadWorldObject(...)
```

generic disk path。

如果调用方：

```text
只 materialize object #17
```

仍然使用已加载的 partition data，不再 range-read sidecar。

---

## 10. Component materialization prerequisite

Phase 4 已要求 `ComponentSchema` 有 generated decode/emplace thunk。

Phase 8 **只能使用该 seam**。

如果不存在：

```text
STOP Phase 8
return to Phase 4
```

禁止在 L3 临时创建：

```text
WorldComponentAdapter
ReflectionMaterializer
ComponentFactoryRegistry
```

---

## 11. WorldMaterializer exact surface

冻结：

```cpp
class WorldMaterializer final
{
public:
    [[nodiscard]] static
    lux::cxx::expected<WorldMaterializer, WorldMaterializeFailure>
    create(
        std::shared_ptr<const world::WorldDescription> world,
        simulation::ecs::ComponentSchemaSet components
    ) noexcept;

    [[nodiscard]]
    lux::cxx::expected<simulation::ecs::Entity, WorldMaterializeFailure>
    object(
        simulation::ecs::Registry& registry,
        world::WorldPartitionObjectView object
    ) const noexcept;

    [[nodiscard]]
    lux::cxx::expected<void, WorldMaterializeFailure>
    partition(
        simulation::ecs::Registry& registry,
        const world::WorldPartitionData& data,
        std::vector<simulation::ecs::Entity>* created = nullptr
    ) const noexcept;
};
```

No additional Plan/Scratch/Binding types。

`ComponentSchemaSet` 传 value；其现有 shared-impl semantics 可保 schema lifetime。

WorldDescription 用 shared_ptr retain。

---

## 12. Materializer cold cache

`WorldMaterializer::create()` 唯一允许的 compiled mapping：

```text
WorldDescription schema ordinal
    -> nullable ComponentSchema pointer/generated decode-emplace operation
```

用 contiguous vector即可。

不要：

```text
unordered_map per object
string lookup per record
RefClass lookup per field
```

Cold create可以按 canonical stable name建立映射。

---

## 13. v1 WorldDataSchema -> ComponentSchema mapping

冻结直接映射规则：

```text
WorldDataSchemaId.name == ComponentSchemaId.name
```

并验证 stable hash/name一致。

结果：

```text
World schema absent from ComponentSchemaSet
    -> mapping entry = null
    -> materialization ignores this record

World schema present
    -> call ComponentSchema generated decode/emplace thunk
```

Why ignore unknown：

```text
headless product can omit visual components
product can link subset of component functionality
```

不要创建 generic required/optional schema policy registry。

如果某 Product 必须强制 schema存在，在 concrete Product/System build validation 中做。

---

## 14. Direct-materializable restrictions

Generic v1 materializer只处理：

```text
self-contained component payload
Registry-owned reversible state
no external side effects
no runtime Entity reference resolution needed
```

明确排除：

```text
component payload requiring WorldObjectId -> Entity resolution
Parent<Entity> relation encoded as WorldObjectId
external Jolt body creation
Render GPU resource creation
Audio handle creation
arbitrary script VM allocation with external lifecycle
```

这些由 concrete System/Probe在 generic object materialization 后处理。

不要因为 hierarchy需求立刻把 WorldObjectId->Entity global index引回 Scene。

同一 partition若 concrete post-process需要 object ordinal -> Entity，可由调用方保存 `created` 并建立 transaction-local table。

---

## 15. Object materialization algorithm

`object()`：

1. `registry.create()`；
2. iterate object records；
3. schema ordinal bounds check；
4. mapping null -> skip；
5. mapping present -> generated decode/emplace；
6. any failure -> `registry.destroy(entity)`；
7. success -> return Entity。

不自动添加：

```text
WorldObjectIdComponent
PartitionOwnershipComponent
PersistentIdComponent
```

除非它们本身就是 World payload 中普通 direct-mapped schema。

---

## 16. Partition materialization atomicity

`partition()`：

```text
created_local = []
for object:
    entity = object(...)
    append created_local
on any failure:
    destroy created_local entities still valid
    if created output != null -> clear
    return failure
success:
    if created output != null -> move/copy handles out
```

Atomic scope = entities/components created by **this invocation**。

Generic materializer不回滚调用前 Registry state。

Gameplay/system不应同时 mutation同一 Registry；调用必须发生在 Simulation-owned safe point。

---

## 17. No implicit ownership after materialization

`WorldMaterializer` 不保存 created entities。

它不知道：

```text
谁以后卸载
谁跟 partition生命周期
NPC是否脱离出生区继续存在
```

调用方 concrete System若需要：

```cpp
std::vector<Entity> my_partition_entities;
materializer.partition(registry, data, &my_partition_entities);
```

是否保存、如何删除由 System决定。

没有：

```text
LoadedPartition generic type
PartitionEntityMap service
Scene residency table
```

---

## 18. Async cancellation contract

Scene teardown不做 per-Scene join。

正确 workflow composition：

```text
concrete System emits typed intent
runtime/product handler captures:
    WorldStorageSource by value
    requester ObjectWeakRef / receiver
    Scene stop token
start loadWorldPartition sender in Host-owned scope

Scene close:
    disconnect new intents
    request stop
    Scene/System can be destroyed

late completion:
    sender-owned source still valid
    weak requester expired -> discard
```

Host shared structured scope仅 Product shutdown join。

禁止：

```text
SceneAsyncScope
SceneProcessScope
SceneRuntimeExecution
per-Scene worker join manager
```

---

## 19. Process boundary

Process owns：

```text
operation execution
byte/range IO
scheduler/execution resources
stop propagation
```

Process source headers禁止 include：

```text
WorldPartitionData
Scene
StreamingSystem
WorldMaterializer
```

L3 通过 `OperationPort<ReadWorldStorageRange>`/Sender composition 使用 Process。

---

## 20. Failure model

`WorldStorageRuntimeFailure` 至少区分：

```text
INVALID_SOURCE
INVALID_PARTITION
INVALID_VOLUME
BUNDLE_MISMATCH
RANGE_OVERFLOW
IO_FAILURE
CORRUPT_DESCRIPTOR
DIGEST_MISMATCH
DECOMPRESSION_FAILURE
DECODE_FAILURE
ALLOCATION_FAILURE
CANCELLED handled as set_stopped where sender contract applies
```

`WorldMaterializeFailure` 至少区分：

```text
INVALID_WORLD_SCHEMA
INVALID_OBJECT
COMPONENT_DECODE_FAILURE
ALLOCATION_FAILURE
```

不要把 backend errno/Vulkan/Jolt error直接暴露到 generic public enum；可保 implementation code/detail。

---

## 21. Tests

### WorldStorageSource

```text
copy Source keeps world/provider alive
Source destruction after operation start does not invalidate operation
BundleId/Generation/VolumeOrdinal verification
invalid range/overflow
```

### loadWorldPartition

```text
single extent
multi extent
multi volume
cancel before start
cancel during IO
cancel during decode
exact bytes read
no object-level IO
```

### WorldMaterializer

```text
schema name direct match
unknown schema ignored
malformed component payload
object rollback
partition rollback
created output success/failure semantics
no automatic WorldObjectId component
no external side effect
no RefClass hot lookup
```

### Lifetime race

```text
start operation
request Scene stop
destroy requester/Scene
complete IO later
no UAF
weak completion discarded
Host scope remains alive
```

Run ASAN/TSAN where supported。

---

## 22. Completion condition

Phase 8结束时，应能在**没有任何 StreamingManager**的情况下做到：

```text
concrete code chooses partition ordinal
-> async load WorldPartitionData
-> return to Simulation Lane
-> optionally materialize selected/all objects
-> concrete code owns resulting Entity policy
```

如果为了做到这件事新增了 `*Manager/*Context/*Binding/*Plan`，必须回退检查是否违反本规范。
