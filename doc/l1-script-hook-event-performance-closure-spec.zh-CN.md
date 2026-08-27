# Lux L1 Script / Hook / Event 性能收敛重构方案

## 1. 最终目标

当前架构的高层方向已经正确：

```text
Simulation
│
├── RenderSystem
│     └── HookPoint
│
├── PhysicsSystem
│     ├── HookPoint
│     └── EventPoint
│
└── ScriptSystem
      ├── consumes HookPoint
      ├── consumes EventPoint
      └── owns all script runtime state
```

本轮不改变这个模型。

重构目标只有四个：

1. 消灭所有随“无关全局规模”增长的 runtime algorithm。
2. 消灭重复 runtime state、重复 registry、重复 identity。
3. 使用现有 `SparseSet`、EnTT、Script ABI、Simulation Description，不重新发明基础设施。
4. 把 runtime 数据组织成可以自然扩展到百万级对象和高 fan-out Hook 的形态。

核心性能合同：

```text
operation                         target complexity
────────────────────────────────────────────────────
Hook connect                      amortized O(1)
Hook disconnect                   O(1)
Hook dispatch                     Θ(actual handlers)

Broadcast Event connect           amortized O(1)
Broadcast Event disconnect        O(1)
Broadcast Event dispatch          Θ(actual deliveries)

Targeted Event target lookup      O(1)
Targeted Event connect            amortized O(1)
Targeted Event disconnect         O(1)
Targeted Event dispatch           O(1) + Θ(target handlers)

Entity -> Script runtime          O(1)
Script attach                     Θ(its bindings)
Script detach                     Θ(its bindings)
Dirty entity insertion            O(1)
Mutation flush                    Θ(actual dirty entities/bindings)
```

允许的线性复杂度只能来自：

> **本次操作真正必须处理的数据。**

严禁：

```text
scan all handlers
scan all instances
scan all mounts
scan all targets
lower_bound over global targets
erase-remove unrelated subscribers
normal mutation 后 global sort
dirty queue linear dedup
full-world fallback resync
per-frame heap allocation
```

---

# 2. 现有代码哪些设计保留

## 2.1 保留 generic HookPoint / EventPoint 边界

现在 `HookPoint` 已经不依赖 Script ABI，只认识 callback/context，这是正确边界。

保持：

```cpp
HookPoint<void(Args...)>
EventPoint<Route, Payload>
```

它们属于：

```text
engine/simulation/system
```

而不是：

```text
engine/simulation/script
```

---

## 2.2 保留 ScriptEndpointBridge

Script ABI 转换应该继续只发生在：

```text
generic endpoint
        ↓
ScriptEndpointBridge
        ↓
lux_script_call_frame
```

当前 `ScriptEndpointBridge` 已经完成这个隔离。

不要把：

```cpp
lux_script_call_frame
BoundScriptCall
ScriptAsset
```

放回 generic `HookPoint/EventPoint`。

---

## 2.3 保留 ScriptSystemDescription

当前持久化模型已经基本正确：

```text
ScriptMountId
Asset UUID
Simulation | Entity(WorldObjectId)
ScriptSymbolId
SystemInstanceId
HookPointId / EventPointId
```

Binding 本身没有 Entity；Simulation-scoped mount 也已经禁止绑定 Entity-targeted Event。

这部分不要重新设计。

特别是不要重新引入：

```text
ScriptComponent persistent data
function name as identity
TargetCatalog
SemanticCatalog
ScriptBindingSession
```

---

## 2.4 保留 `BoundScriptCall`

继续保持：

```cpp
struct BoundScriptCall {
    lux_script_invoke_fn invoke;
    void* context;
};
```

它作为最终 executable call record 是非常好的。

目标仍然是：

```text
cold path:
Asset / Symbol / Type / Backend resolution

hot path:
BoundScriptCall
```

---

# 3. 第一项核心重构：HookPoint 改成 Sparse-Dense storage

当前 HookPoint 使用：

```text
vector<Handler>
vector<Mutation>
monotonic token

disconnect:
    find_if handlers

flush:
    remove_if handlers
```

因此单次删除和 mutation flush 都可能成为 O(N)。

对于高 fan-out Hook 不可接受。

---

## 3.1 不重新造容器

仓库已经在 `InputActionRegistry` 使用：

```cpp
lux::cxx::OffsetAutoSparseSet<
    uint32_t,
    InputActionDesc,
    1
>
```

而且已经提供：

```cpp
insert()
erase()
contains()
at()
keys()
values()
```

以及 runtime allocated ID。

因此 HookPoint 应优先直接复用 `lux::cxx::SparseSet` 家族。

不要实现：

```text
HookSlotMap
HookDenseMap
HookArena
HookRegistryStorage
```

这种重复容器。

---

## 3.2 目标 HookPoint

概念上：

```cpp
template<class... Args>
class HookPoint<void(Args...)>
{
    struct Handler
    {
        void* context;
        Callback callback;
    };

    OffsetAutoSparseSet<uint32_t, Handler, 1> handlers_;
};
```

connect：

```cpp
auto id = handlers_.insert({
    context,
    callback
});
```

disconnect：

```cpp
handlers_.erase(id);
```

dispatch：

```cpp
for (const auto& handler : handlers_.values())
    handler.callback(handler.context, args...);
```

hot record 仍然只有：

```text
2 pointers = 16 bytes
```

---

# 4. Registration ID 必须解决 stale handle

这里唯一需要确认的是现有 `SparseSet` 对 ID reuse 的语义。

如果：

```text
erase(42)
insert(...)
→ 再次得到 42
```

那么旧 handle 可能误删新 subscriber。

因此 connection handle 必须具有 generation protection。

但是：

> 不应该因此再实现一套 SlotMap。

最佳路径是：

### 如果 lux::cxx SparseSet 已有 generation 能力

直接使用。

### 如果没有

只给基础 SparseSet 增加最小 generation support，或者让 endpoint 维护与 sparse slot 对齐的 generation array。

继续沿用现有公共名称：

```cpp
struct EndpointConnectionToken
```

即可。

不需要同时出现：

```text
EndpointConnectionToken
HookRegistrationId
HookSubscriptionId
HandlerHandle
RuntimeHookHandle
```

五个相同语义类型。

建议 token 内部逻辑：

```cpp
struct EndpointConnectionToken
{
    uint32_t slot;
    uint32_t generation;
};
```

---

# 5. 删除 HookPoint 内部 Mutation Queue

当前：

```text
connect
disconnect
    ↓
mutations_
    ↓
flushMutations()
```

这实际上重复实现了一套 structural mutation system。

而 Simulation 本来就具有 safe-point / scheduler ownership。

ScriptSystem 自己也已经通过 EnTT 信号做 deferred mutation。

因此应该冻结：

> **Endpoint subscription topology 只能在 Simulation quiescent point 修改。**

也就是说：

```cpp
connect()
disconnect()
```

在 dispatch active 时直接：

```text
DISPATCH_ACTIVE
```

否则立即 O(1) 修改 SparseSet。

不再需要：

```cpp
Mutation
mutations_
mutation_capacity_
flushMutations()
```

这样同时解决三个问题：

1. 删除 linear mutation scans。
2. 减少一个状态机。
3. 根治目前 ScriptSystem shutdown 中 disconnect 尚未 commit 就释放 bucket 的 lifetime 风险。

当前 HookPoint/EventPoint 内部 mutation queue 可以直接删除。

---

# 6. Hook dispatch 的正式 contract

默认：

> **Subscriber ordering unspecified。**

这是得到：

```text
O(1) insert
O(1) swap-pop erase
contiguous dispatch
```

的关键。

不要为了现在没有明确需求的 deterministic subscriber ordering 在每次 mutation 后：

```cpp
std::sort(...)
```

如果未来某个 endpoint 真正要求 order，再单独增加显式 capability。

但不要让所有 HookPoint 为此支付成本。

---

# 7. EventPoint 必须重构

当前 generic Entity-targeted EventPoint 是：

```text
for occurrence
    for every handler
        compare target
```

复杂度是：

```text
O(events × all handlers)
```



这是当前 generic endpoint 最大的 scalability 问题之一。

---

# 8. Broadcast Event

Broadcast 很简单：

```text
occurrences[]
handlers SparseSet
```

drain：

```cpp
for (const auto& event : occurrences)
    for (const auto& handler : handlers.values())
        handler(...);
```

复杂度：

```text
Θ(actual deliveries)
```

因为每个 broadcast event 本来就必须送达所有 subscriber。

这是理论最优。

---

# 9. Entity-targeted Event

Entity route 不应该扫描 subscriber。

结构应变成：

```text
EventPoint
│
├── all_handlers
│     // connectAll()
│
└── target index
      Entity
        ↓ O(1)
      TargetBucket
        ↓
      SparseDense handlers
```

dispatch：

```text
Occurrence(Entity42)

1. invoke all_handlers
2. sparse lookup Entity42
3. iterate Entity42 bucket
```

复杂度：

```text
O(1) + Θ(all-observers + Entity42 subscribers)
```

与世界里存在多少其它 Entity 无关。

---

# 10. 不需要 generic arbitrary Target index framework

当前实际 L1 需求是：

```cpp
EntityTargetedRoute<ecs::Entity>
```

不要为了未来可能出现的：

```text
NetworkPeerTarget
AudioBusTarget
AnimationBoneTarget
```

现在设计：

```text
ITargetIndex
TargetIndexPolicy
TargetLookupStrategy
TargetBucketProvider
```

等抽象。

直接给：

```cpp
EventPoint<EntityTargetedRoute<ecs::Entity>, Payload>
```

做高性能 specialization。

如果以后真的出现另一类大规模 target，再增加对应 specialization。

---

# 11. Event producer storage 继续保留，但线程 contract 要收紧

当前 EventPoint 的：

```text
producer_count
per-producer vector
```

是很好的 deterministic event buffering 思路。

继续保持：

```text
producer 0 → private buffer
producer 1 → private buffer
producer 2 → private buffer
```

record：

```text
amortized O(1)
```

drain 在 TaskGraph barrier 后执行。

但是不要继续用一个普通：

```cpp
size_t active_writers_
```

去模拟线程同步。

正确模型应该是：

> scheduler 保证每个 producer buffer 的 exclusive ownership，并保证 drain 与 record phase 不重叠。

Release build 不需要原子计数。

Debug build 可以有 assertion/epoch validation。

这样没有：

```text
mutex
atomic on every event
shared vector contention
```

---

# 12. EventPoint::prepare 的 P0 必须同时修复

即使后续删除大量 defensive state，目前：

```text
prepare()
```

至少必须拒绝 drain-active 状态。

当前 `prepare()` 没检查 `draining_`，callback reentrant prepare 可以 invalidate 正在 drain 的 vector。

短期先修：

```cpp
if (draining_)
    return EEndpointMutationError::DISPATCH_ACTIVE;
```

长期按上述 safe-point contract 简化。

---

# 13. ScriptSystem 不再使用“Instance vector + DEAD scan”

当前：

```cpp
allocateInstance()
    find_if(state == DEAD)
```

属于 O(N)。

但这里其实没有必要引入：

```text
ScriptInstancePool
EntityScriptInstanceTable
RuntimeInstanceManager
```

因为现有 Description 已经给出了更简单的事实：

> 一个 `ScriptMountDescription` 在任意时刻最多对应一个 active runtime instance。

因此最简洁的设计是：

```text
one RuntimeMount per ScriptMountDescription
```

---

# 14. 用 RuntimeMount 直接替代独立 Instance allocator

私有实现：

```cpp
struct RuntimeMount
{
    const ScriptMountDescription* authored;

    ScriptInstanceScope scope;

    ResolvedScriptAsset asset;

    const ScriptBackendDescriptor* backend;
    ScriptBackendInstance backend_instance;

    uint32_t first_binding;
    uint32_t binding_count;

    uint32_t first_method;
    uint32_t method_count;

    State state;
    EBehaviorStopReason stop_reason;
};
```

然后：

```cpp
std::vector<RuntimeMount> mounts_;
```

大小在 `prepare()` 时直接：

```cpp
mounts_.resize(description.mounts().size());
```

从此：

```text
persistent mount ordinal
        =
runtime mount slot
```

不需要：

```text
InstanceId
Instance allocator
DEAD scan
instance lookup table
```

这是一次重要的类型删除。

---

# 15. Runtime Binding 使用全局扁平数组

不要：

```cpp
RuntimeMount {
    vector<Binding>
    vector<PreparedMethod>
}
```

百万 mount 时意味着大量小 heap allocation。

应该在 prepare/link 阶段一次计算：

```text
total binding count
total unique prepared-method count
```

然后：

```cpp
std::vector<RuntimeBinding> bindings_;
std::vector<PreparedMethod> methods_;
```

每个 RuntimeMount 只保存：

```text
first + count
```

布局：

```text
mounts[]
│
├─ mount 0 -> bindings [0,4)
├─ mount 1 -> bindings [4,7)
└─ mount 2 -> bindings [7,12)

bindings[]
────────────────────────────────

methods[]
────────────────────────────────
```

只产生几个大型 contiguous allocations。

---

# 16. RuntimeBinding

只需要一个私有结构：

```cpp
struct RuntimeBinding
{
    enum class Kind : uint8_t
    {
        Hook,
        Event
    };

    Kind kind;

    uint32_t bucket;
    uint32_t method;

    EndpointConnectionToken registration;

    lux::script::ScriptSymbolId symbol;
};
```

甚至 `Kind` 如果 endpoint bucket namespace 可以编码，也可以省略。

不要创建公共：

```text
CompiledScriptBinding
ResolvedScriptBinding
LinkedScriptBinding
RuntimeScriptBindingHandle
```

多层模型。

Description → `RuntimeBinding` 一次转换就够了。

---

# 17. prepare() 应成为真正的一次 Link/Compile phase

当前 ScriptSystem 仍大量执行：

```text
findBackend
findHook
findEvent
findHookBucket
findEventBucket
findMethod
find export symbol
```



应该改成：

```text
Persistent IDs
       ↓
ScriptSystem::prepare()
       ↓
small dense runtime slots
```

prepare 完成以后：

```text
SystemInstanceId
HookPointId
EventPointId
ScriptSymbolId
WorldObjectId
```

都不再出现在 normal runtime routing path。

---

# 18. Backend lookup 直接数组化

当前 backend 是：

```cpp
find_if(backends, kind)
```

但：

```cpp
rdesc::Script::Kind
```

是小 enum。

直接：

```cpp
std::array<
    const ScriptBackendDescriptor*,
    kScriptKindCount
> backends_;
```

lookup：

```cpp
backend = backends_[to_index(kind)];
```

严格 O(1)。

没有理由 hash，更没有理由 scan。

---

# 19. Endpoint lookup 只存在于 prepare

Persistent key：

```text
(SystemInstanceId, HookPointId)
(SystemInstanceId, EventPointId)
```

在 prepare 阶段构造一次 lookup index。

可以使用项目已经存在的 hash/container infrastructure。

不要自己实现：

```text
EndpointHashTable
EndpointResolver
EndpointCatalog
```

prepare 输出：

```text
binding.bucket = 7
```

之后：

```cpp
hooks_[7]
```

即可。

因此 endpoint lookup 的实现细节不会进入 hot runtime。

---

# 20. Script Symbol lookup 也只允许一次

现在每个 attach 仍然会：

```cpp
find_if(asset.description.exports,
        symbol_id)
```



应该在 module/asset resolution 后一次建立 symbol→export index。

如果当前 Script asset ledger 已经具有 lookup API：

> 使用现有 ledger。

不要再建第二套 ScriptSymbolRegistry。

如果没有，则 backend/module cold state 建一个 index。

一次完成。

---

# 21. HookPointScriptTable 不需要成为新 public class

我们之前讨论过：

```text
HookPointScriptTable
```

这个语义仍然成立。

但当前代码已经有：

```text
ScriptSystem::State::HookBucket
```

所以没有必要为了名字重新增加：

```text
HookPointScriptTable.hpp
HookPointScriptTable.cpp
IHookPointScriptTable
```

更简单的是：

```cpp
struct HookBucket
{
    endpoint;
    connection;
    SparseSet<Handler> handlers;
};
```

然后：

```cpp
std::vector<HookBucket> hooks_;
```

这本质上就是 ScriptSystem 的 HookPointScriptTable。

**语义成立即可，不需要 public type。**

---

# 22. Script Hook Bucket 自身也使用 SparseSet

当前：

```cpp
vector<Handler>
```

然后 detach instance：

```text
遍历所有 Hook buckets
    erase-remove handlers
```

应改成：

```cpp
OffsetAutoSparseSet<uint32_t, Handler, 1> handlers;
```

每个 binding 得到 registration ID。

Script hook dispatch：

```cpp
for (const auto& handler : handlers.values())
    invoke(handler);
```

仍然 contiguous。

---

# 23. Script instance 不需要自己分配 vector 保存 registrations

因为：

```text
RuntimeMount
  ↓
RuntimeBinding range
```

已经是天然 owner。

每个 `RuntimeBinding` 中保存：

```cpp
registration
```

detach：

```cpp
for (auto& binding : mountBindings(runtime_mount))
{
    if (binding.kind == Hook)
        hooks_[binding.bucket].handlers.erase(binding.registration);
    else
        ...
}
```

复杂度：

```text
Θ(this mount's binding count)
```

这是理论最优。

而且不需要：

```text
vector<HookRegistration>
vector<EventRegistration>
SmallVector
registration arena
```

额外数据结构。

---

# 24. Script targeted Event 去掉 lower_bound

当前 Script Event bucket 已经比旧架构好很多，但仍然：

```text
sorted EntityRange
lower_bound(Entity)
```



目标应该是：

```text
Entity
 ↓ O(1)
EntityBucket
 ↓
dense script handlers
```

优先复用：

```text
lux::cxx SparseSet
```

如果其 key policy 不适合 `ecs::Entity`，则直接使用 EnTT 自己的 sparse-set mechanism。

不要写新的 Entity hash table。

---

# 25. Script Entity lookup 不允许 scan instances

当前 reconcile(entity) 会遍历 instances。

重构后甚至不需要 Instance lookup：

private ECS component：

```cpp
struct ScriptAttachment
{
    uint32_t mount_slot;
};
```

于是：

```text
Entity
 ↓
ScriptAttachment
 ↓
mounts_[mount_slot]
```

O(1)。

WorldObjectId 不再作为 normal runtime lookup key。

---

# 26. ScriptAttachment 只做 runtime backlink

当前：

```cpp
struct ScriptAttachment {
    WorldObjectId object;
};
```



建议改成：

```cpp
struct ScriptAttachment
{
    uint32_t mount_slot;
};
```

WorldObjectId 的作用应该结束于：

```text
Description
   ↓
World resolve/link
   ↓
Entity
```

之后 reactive runtime 不应该再拿 UUID 去 description 里搜索。

---

# 27. EnTT responsive lifecycle 收敛

EnTT callback 永远只做：

```text
O(1) mark dirty
```

例如：

```cpp
on_destroy<ScriptAttachment>(entity)
{
    dirty_.insert(entity);
}
```

不要：

```text
detach backend
remove handlers
resolve asset
scan mounts
```

这些都留到：

```cpp
ScriptSystem::flushMutations()
```

的 Simulation safe point。

---

# 28. Dirty queue 使用 SparseSet，不要 vector + find

当前：

```cpp
std::find(dirty_current, entity)
```

是 O(N)，大量 structural mutation 会 O(N²)。

直接：

```cpp
SparseSet<Entity> dirty_entities;
```

语义：

```text
markDirty       O(1)
already dirty   O(1)
iterate         Θ(dirty)
clear           Θ(dirty)
```

不要：

```text
dirty_current
dirty_processing
full_resync
linear dedup
```

双 buffer 只有在 flush 过程中可能继续产生 mutation 时才需要。

如果需要：

```text
dirty_current SparseSet
dirty_next    SparseSet
```

即可。

---

# 29. 删除 full_resync fallback

现在 capacity overflow 后：

```text
full_resync = true
→ scan active instances
→ scan attachments
```

这等于：

> 系统负载最高时退化成全世界扫描。

不符合 Lux 的目标。

应该删除。

如果 structural mutation storage 不够：

- 使用能够增长的 SparseSet；
- 或明确返回 capacity error；
- 或预分配到 entity-mount 最大数量。

不能悄悄进入：

```text
O(world size)
```

fallback。

---

# 30. ScriptSystemCapacities 应大幅删除

当前：

```cpp
instances
prepared_methods
hook_buckets
event_buckets
handlers
failures
mutations
```

大多数其实都能从 `ScriptSystemDescription` 精确推导。

例如：

```text
max mounts
    = description.mounts().size()

max handlers
    = total bindings

hook/event bucket upper bound
    = unique endpoints referenced

prepared methods
    = unique symbols per mount
```

因此 runtime caller 不应该重复填写一遍事实。

建议最终：

```cpp
struct ScriptSystemOptions
{
    std::size_t failure_capacity;
};
```

甚至 failure diagnostics 可以用固定 ring，Options 也未必需要。

原则：

> 可以从 immutable Description 推导的数据，不应该再作为 runtime configuration 重复保存。

---

# 31. ScriptSystem prepare 最终流程

```text
1. inspect ScriptSystemDescription

2. calculate:
      mount count
      binding count
      unique method count
      used Hook endpoints
      used Event endpoints

3. allocate exact contiguous runtime arrays

4. link stable endpoint IDs
      → runtime bucket index

5. resolve script assets/modules

6. map SymbolId
      → export/method slot

7. fill RuntimeMount / RuntimeBinding / PreparedMethod

8. build Hook/Event script buckets

9. attach exactly ONE ScriptSystem lane
   to every used generic endpoint

10. instantiate active mounts

11. publish prepared runtime

12. scheduler may start
```

prepare 可以有 allocation、hashing、validation。

运行期不允许再做这些工作。

---

# 32. Script invocation hot path

最终 Hook：

```text
RenderSystem::hook.dispatch(args)
      ↓
generic HookPoint dense Handler[]
      ↓
ScriptEndpointBridge
      ↓
build ABI slots ONCE
      ↓
Script HookBucket dense Handler[]
      ↓
BoundScriptCall.invoke
```

重要的是：

> ABI frame packing 一次 Hook emission 做一次，而不是每个 script subscriber 做一次。

因此保留“一条 Script lane”是正确优化。

---

# 33. Handler 热数据不要塞 diagnostics

当前 Handler 里有：

```text
Instance*
BoundScriptCall
mount
binding_ordinal
target
```

Hook hot path 没必要携带这么多。

建议至少：

```cpp
struct HookHandler
{
    RuntimeMount* owner;
    BoundScriptCall call;
};
```

需要诊断的：

```text
symbol
endpoint
mount ID
```

放在 `RuntimeBinding` cold sidecar。

fault 时从 registration/binding 找 sidecar。

不要为了 rare error 扩大所有 hot handler。

---

# 34. Fault policy

invoke failure：

```text
RuntimeMount.state = Faulted/Retiring
```

然后：

```text
queue mount for retirement
```

不要在 dispatch 中修改 SparseSet。

下一个 safe point：

```text
for own bindings
    O(1) erase each registration

stop backend
release methods
destroy backend instance
release asset
```

复杂度：

```text
Θ(faulting mount's bindings)
```

---

# 35. Shutdown 必须成为严格 safe-point operation

当前 teardown 可能在 endpoint flush 失败后仍然释放 buckets，这是 lifetime 风险。

新模型删除 endpoint deferred mutation 后，shutdown contract 可以非常简单：

```text
scheduler quiesced
no Hook dispatch active
no Event drain active
no Event writers active

        ↓

disconnect Script lanes O(number of used endpoints)
destroy runtime mounts
remove ScriptAttachment
clear runtime arrays
```

如果 endpoint 返回 `DISPATCH_ACTIVE`：

```text
shutdown fails
```

绝不能继续 destroy context。

---

# 36. ScriptAttachment ownership 必须闭环

ScriptSystem 创建：

```cpp
ScriptAttachment
```

ScriptSystem 就必须删除它。

shutdown：

```text
release EnTT observer connection
remove all ScriptAttachment written by this ScriptSystem
destroy runtime
```

prepare 失败 rollback 也一样。

最终 invariant：

> 没有 ScriptSystem，就没有 Script runtime projection。

---

# 37. 生命周期保持一套

不要出现：

```text
GlobalScriptInstance
EntityScriptInstance
```

两套状态机。

RuntimeMount 统一：

```text
Inactive
Constructing
Active
Retiring
Faulted
```

scope：

```text
Simulation
Entity
```

只是 context。

backend：

```text
createInstance
prepareMethod
startInstance
stopInstance
releaseMethod
destroyInstance
```

当前 backend contract 已经留有这些 seam。

但三个 concrete backend 目前基本没有真正实现 start/stop。

下一阶段要么正式实现，要么从 v1 contract 暂时删除 optional lifecycle。

不要维持“API 看似支持、backend 实际全 nullptr”的半合同。

---

# 38. RuntimeObject 的明确裁决

`lux::meta::RuntimeObject` 当前是：

```text
reflection-aware
type-erased
value holder
16-byte SBO
heap fallback
RefClass construction/destruction
```



FlowForge 用它保存动态 constant value 是合理场景。

### Script runtime core 不使用 RuntimeObject

不要用于：

```text
Hook Handler
Event Handler
RuntimeBinding
registration handle
Script RuntimeMount
Native state
CppStatic instance owner
```

原因：

- 带 reflection dependency；
- RefClass object 会进入 heap；
- object identity 与 runtime registration 完全不是一回事；
- 不适合作为百万级 script instance allocator。

### 可以使用的地方

```text
FlowForge constant
Editor property
authoring default value
cold-path reflected state migration
inspection/debugging
```

所以答案是：

> RuntimeObject 有价值，但不是本次 Script runtime 优化应该复用的轮子。

---

# 39. Backend 层也要消灭 linear scans

## CppStatic

当前 descriptor resolution 有多个 fallback scan。

改成：

```text
descriptor_key → descriptor*
```

一次建立 index。

`descriptor_key` 是 authoritative identity。

删除：

```text
only descriptor fallback
exports equality fallback
linear candidate scan
```

这种模糊恢复逻辑。

---

## Native

当前 module cache：

```text
vector<ModuleEntry>
find_if(asset UUID)
```



改成：

```text
AssetId -> ModuleEntry
```

existing module reuse O(1) expected。

不要创建通用 `ScriptModuleStore`，因为 Native module caching 是 backend-specific executable state。

保持 backend 内部即可。

---

## Lua

当前 prototype：

```text
vector<Prototype>
linear find AssetId
```

以及 component：

```text
vector<LuaComponentBinding>
linear find name
```



全部换成 backend 内部 lookup index。

尤其：

```text
has_component("Transform")
get_component("Transform")
```

不能每次线性扫描 component contracts。

---

# 40. Backend instance allocation 第二阶段池化

三个 backend 当前都存在不同程度的 per-instance heap allocation。

极大场景最终不可接受。

但不要为此造：

```text
UniversalScriptObjectPool
ScriptMemoryManager
RuntimeObjectArena
```

每个 backend 对自己的 object layout 最清楚，因此 pool 应 backend-local。

### CppStatic

每个 reflected descriptor 建 typed-size slab。

### Native

每个 `(state_size,state_align)` module 建 state slab。

### Lua

C++ Instance wrapper 用预留 vector + free-list；Lua table 内存交给 Lua allocator / arena policy。

核心 ScriptSystem 不管理这些内存。

---

# 41. 一个非常有价值的简单技巧：预留 vector + free-list

并不是所有地方都必须上新容器。

例如 backend Instance 对象如果需要稳定 pointer：

```cpp
instances.reserve(max_instances);
```

然后永远不超过 capacity。

地址就不会因 vector reallocation 改变。

增加：

```text
free_indices[]
```

即可：

```text
allocate   O(1)
free       O(1)
stable pointer
```

这比新写：

```text
PagedObjectPool
ScriptInstanceArena
```

更简单。

只有 object state 本体尺寸可变时才使用 slab。

---

# 42. Collision 等离散事件最终路径

以 Physics Collision 为例：

```text
Physics worker
    ↓
EventPoint.record(entity, CollisionEvent)
    O(1)

Physics barrier
    ↓
EventPoint.drain()

Entity sparse index
    ↓ O(1)
target handlers

        ├─ Native subscriber
        └─ ScriptSystem lane
                ↓
           Script Event bucket
                ↓ O(1)
           Entity script handlers
                ↓
           BoundScriptCall
```

没有：

```text
ScriptSystem::dispatchTo
global event registry
entity scan
script scan
name lookup
```

`EventPointSpec.dispatch_hook` 可以继续定义事件在某个 System execution barrier 被 drain 的位置。

这样 Collision 仍然是 Event semantic，而不是伪装成 Hook。

---

# 43. 不要引入 ImmediateEvent / BufferedEvent 两套 public 类型

当前多 producer event buffering 对 Simulation determinism 很有价值。

第一版继续：

```text
record occurrence
drain at declared dispatch hook
```

即可。

如果未来证明某种 event 必须 synchronous immediate delivery，再增加 delivery policy。

现在不要提前发明。

---

# 44. Lua structured event 是必须补的能力

Generic EventPoint 已经支持 arbitrary semantic Payload，但 Lua backend 当前只接受几个 scalar ABI kind。

因此真实：

```cpp
CollisionEvent
DamageEvent
AnimationNotifyEvent
```

目前无法自然送入 Lua。

应增加：

```text
semantic record
    ↓
backend-local Lua record marshaller
```

但是禁止：

```text
Global SemanticCatalog
Script Type Registry
Runtime Reflection Router
```

可以复用当前 semantic type + Script ABI descriptor。

marshaller 只属于 Lua backend。

---

# 45. 公共类型最终应控制在这个数量

Generic Simulation：

```text
HookPoint
EventPoint
HookPointSpec
EventPointSpec
EndpointConnectionToken
```

Script description：

```text
ScriptSystemDescription
ScriptMountDescription
ScriptBindingDescription
ScriptMountScope
ScriptBindingTarget
```

Script runtime public：

```text
ScriptSystem
ScriptBackendDescriptor
ScriptBackendInstance
ScriptEndpointDescriptor
ScriptBehavior
```

ABI：

```text
BoundScriptCall
lux_script_call_frame
```

其它：

```text
RuntimeMount
RuntimeBinding
PreparedMethod
HookBucket
EventBucket
```

全部 private implementation。

不要把 private runtime layout 升级成公共架构名词。

---

# 46. 文件级调整

## 保留并重写内部

```text
engine/simulation/system/include/.../HookPoint.hpp
engine/simulation/system/include/.../EventPoint.hpp

engine/simulation/script/include/.../ScriptSystem.hpp
engine/simulation/script/include/.../ScriptBackend.hpp
engine/simulation/script/include/.../ScriptEndpointBridge.hpp

engine/simulation/script/src/ScriptSystem.cpp
```

## 基本保留

```text
ScriptSystemDescription.hpp
ScriptSystemDescription.cpp
ScriptSystemDescriptionCodec.*
```

Description 不应该因为 runtime performance refactor 大幅变化。

## Backend 局部优化

```text
cpp_static/*
lua/*
native/*
```

---

# 47. 不要新建这些文件

明确禁止实现 LLM 自动生成：

```text
HookPointManager.*
HookPointRegistry.*
HookPointScriptTable.*          // 若只是包装 hooks_ vector
EventManager.*
EventRouter.*
ScriptRuntimeManager.*
ScriptInstanceManager.*
ScriptModuleStore.*
ScriptBindingCompiler.*
ScriptMutationManager.*
ScriptObjectPool.*              // generic one-size-fits-all
RuntimeObjectAdapter.*
ScriptTargetResolver.*
ScriptSemanticCatalog.*
```

除非实现过程中证明某个类型拥有独立 lifetime + 独立 invariant，并且不能作为已有 owner 的 private data。

默认答案是：

> 不创建。

---

# 48. ScriptSystem.cpp 是否拆文件

当前 `ScriptSystem.cpp` 接近 46 KB，本身说明内部职责仍然偏多。

但不要通过公共 class 拆。

如果重构完成后仍然很大，可以只做 source-level split：

```text
ScriptSystem.cpp
ScriptSystemLink.cpp
ScriptSystemRuntime.cpp
```

共享一个 private：

```text
src/ScriptSystemState.hpp
```

不安装，不 public。

如果重构后文件已经自然缩小，则连这个 split 都不要做。

---

# 49. 分阶段实施顺序

## Phase 0 — Correctness closure

先修：

```text
EventPoint::prepare during drain
ScriptSystem shutdown endpoint lifetime
ScriptAttachment failed-prepare/shutdown cleanup
```

不要把 correctness bug 与性能重构混成一个巨大 patch。

---

## Phase 1 — Generic endpoint storage

只改：

```text
HookPoint
EventPoint
endpoint tests
```

完成：

```text
Sparse-dense Hook handlers
O(1) registration
O(1) erase
O(1) Entity target lookup
remove endpoint mutation queues
safe-point contract
```

ScriptSystem 暂时只适配 API。

---

## Phase 2 — ScriptSystem flat runtime layout

完成：

```text
RuntimeMount = mount ordinal
flat RuntimeBinding
flat PreparedMethod
direct bucket slots
SparseSet Hook handlers
Sparse target Event buckets
Sparse dirty entities
remove full_resync
remove global scans
```

这一阶段结束后，`ScriptSystem` runtime 不应有任何按全局 instances/handlers 查找的算法。

---

## Phase 3 — Backend indexes

完成：

```text
CppStatic descriptor direct index
Native AssetId module index
Lua AssetId prototype index
Lua component index
```

不改变 public backend interface。

---

## Phase 4 — Backend allocation

benchmark 以后处理：

```text
CppStatic object slabs
Native state slabs
Lua wrapper free-list
prepared-call pools
```

这里才优化 heap count。

不要在前面几阶段提前造 allocator framework。

---

## Phase 5 — Structured payload

给 Lua 补 record/event payload。

验证：

```text
Physics CollisionEvent
→ Entity-targeted EventPoint
→ Lua entity script
```

完整闭环。

---

# 50. 必须建立性能 contract tests

普通 unit test 不够。

需要明确规模测试。

### HookPoint

```text
1,000,000 handlers

insert random handler
    must not inspect existing 1M

erase random handler
    must not inspect existing 1M

dispatch
    exactly 1M calls
    zero allocations
```

---

### Targeted Event

```text
1,000,000 subscribers
spread across many Entities

emit to Entity42
```

必须只访问：

```text
Entity42 index
Entity42 handlers
connectAll handlers
```

不能访问其它 Entity subscriber。

---

### Script detach

```text
1,000,000 total script bindings
target mount owns 4 bindings
```

detach work 应接近：

```text
4 × O(1)
```

而不是随着 1M total bindings 增长。

---

### Dirty entities

```text
100,000 distinct EnTT structural events
```

不能出现 O(N²) dedup。

---

# 51. Allocation tests

正式 runtime 后：

```text
Hook dispatch         0 allocations
Event record          0 allocations after prepare/reserve
Event drain           0 allocations
Script Hook dispatch  0 allocations
Script Event dispatch 0 allocations
dirty mark            0 allocations after reserve
detach                0 general allocations
```

backend script invocation：

- Native：0
- CppStatic：0
- Lua：Lua VM 自己的 allocation 要单独测量和控制

---

# 52. Negative implementation gates

最终 production code 中，不允许在 normal runtime path 出现：

```text
std::find
std::find_if
std::remove_if across global runtime collections
std::lower_bound for Entity dispatch
std::sort after ordinary mutation
full_resync
scan all instances
scan all handlers
scan all mounts
scan all endpoints
```

注意：

这些算法可以存在于：

```text
Description validation
prepare/link
authoring
codec
tests
```

但是不能存在于：

```text
dispatch
record
attach
detach
dirty processing
entity event routing
fault retirement
```

---

# 53. 最终架构

```text
PERSISTENT
────────────────────────────────────

SimulationDescription

ScriptSystemDescription
    └── ScriptMountDescription[]
          ├── Asset UUID
          ├── ScriptMountScope
          └── ScriptBindingDescription[]
                ├── ScriptSymbolId
                └── stable endpoint IDs


PREPARE / LINK
────────────────────────────────────

stable IDs
    ↓
RuntimeMount[]
RuntimeBinding[]
PreparedMethod[]
HookBucket[]
EventBucket[]

all dense runtime slots


SIMULATION RUNTIME
────────────────────────────────────

System
 ├── HookPoint
 │     └── SparseDense handlers
 │
 └── EventPoint
       ├── broadcast dense handlers
       └── Entity sparse index
               ↓
           dense handlers


ScriptSystem
 ├── RuntimeMount[]
 ├── RuntimeBinding[]
 ├── HookBucket[]
 │       └── SparseDense script handlers
 │
 ├── EventBucket[]
 │       └── Entity sparse index
 │
 └── dirty Entity SparseSet


HOT PATH
────────────────────────────────────

Hook
→ contiguous generic handlers
→ one Script lane
→ contiguous BoundScriptCall handlers

Target Event
→ O(1) Entity sparse lookup
→ exact subscribers
→ Script lane
→ O(1) Entity sparse lookup
→ exact script subscribers
```

---

# 54. 最终设计哲学

整个方案可以压缩为六条：

**第一，Persistent Identity 与 Runtime Address 分离。**

UUID / SymbolId / EndpointId 只负责持久化和 link。

运行后全部变成 dense runtime slot。

**第二，Sparse ownership + Dense execution。**

Sparse 负责：

```text
O(1) identity
O(1) removal
```

Dense 负责：

```text
cache-friendly iteration
```

这应该成为 Lux runtime container 的核心模式。

**第三，不扫描无关数据。**

任何运行期算法只允许访问：

```text
target object
target bucket
actual recipient
actual mutation
owned binding
```

**第四，Safe-point replaces defensive complexity。**

Simulation scheduler 已经知道什么时候安全。

不要让每个 Hook/Event 再实现一套 transaction/mutation framework。

**第五，Backend owns backend-specific complexity。**

Lua、Native、CppStatic 的 module cache、object pool、marshaller 各自留在 backend。

不要创建通用 God Script Runtime。

**第六，复用 mechanism，不复用不匹配的 abstraction。**

`SparseSet` 应复用。

EnTT 应复用。

Script ABI 应复用。

`RuntimeObject` 不因为“已经存在”就强行用于 script runtime object。

---

# 55. Freeze 条件

完成后只有满足以下条件，我才建议把 Script L1 标为 architecture freeze：

```text
[ ] Hook connect/disconnect O(1)
[ ] Hook dispatch only Θ(actual subscribers)

[ ] Entity Event lookup O(1)
[ ] Entity Event dispatch only Θ(actual target subscribers)

[ ] Script attach/detach only Θ(own bindings)
[ ] Entity → Script runtime O(1)
[ ] dirty insertion O(1)

[ ] no full_resync
[ ] no normal-runtime global sort
[ ] no normal-runtime global scan

[ ] no per-dispatch allocation
[ ] no duplicate registry/catalog

[ ] ScriptSystem safely owns/removes ScriptAttachment
[ ] endpoint teardown cannot leave dangling lane context

[ ] global/entity scripts use same RuntimeMount path
[ ] Collision-style Event complete through Native + Lua

[ ] existing SparseSet is reused
[ ] RuntimeObject stays outside hot runtime
```

最终我建议把这次工作定义为：

> **L1 Script/System Endpoint Scale Closure**

而不是“Script architecture v2/v3”。

因为现在概念架构已经够好了。

真正需要完成的是：

> **把现在正确的 ownership 模型，落实成 scale-independent 的 runtime data structure。**

