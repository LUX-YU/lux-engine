# L1 Simulation Runtime、SystemRegistry Type Catalog 与 SimulationBuilder 实施规范（Baseline v3.1）

> 状态：**Normative construction spec**。
>
> 施工 Phase：**5**。
>
> 前置：Phase 1 `TaskDependencies`、Phase 4 ECS prerequisites 已完成。
>
> 影响：`engine/domain/simulation/description`、`engine/domain/simulation/system`、Simulation runtime leaf。
>
> 本文故意**不设计** `TimeDomain/TickGroup/ScenePhase`；这些 production API 在 architecture probes 前禁止创建。

---

## 1. Final responsibilities

```text
SimulationDescription
    = durable Systems/configuration/dependencies/hooks/events

SystemRegistry
    = cold System TYPE registration catalog

SimulationBuilder
    = one Simulation's cold construction seam

Simulation
    = private concrete System instances
      + private EcsCommandBuffer when command tasks exist
      + compiled synchronous TaskGraph

Registry instance
    = Scene-owned authoritative mutable ECS state
```

Simulation 不负责：

```text
World IO
World materialization/dematerialization
partition lifecycle
asset residency
Render ownership
wall-clock ownership
Scene main loop
```

---

## 2. Public Type Budget

本 Phase 只允许新增/重定义这些 public production types：

```text
SystemRegistration
SystemRegistry              # type catalog, not instance owner
SimulationBuilder
Simulation                  # existing/new runtime type
SystemBuildFailure          # paired build failure
SystemRegistrationFailure   # paired registration failure
SimulationExecutionFailure  # paired execute failure
```

不得新增：

```text
SystemFactoryRegistry
SystemFactory
SystemContext
SystemServices
SimulationContext
SimulationServices
DependencyInjectionContainer
SystemInstanceRegistry
RuntimeResourceRegistry
SystemTaskRegistry
SystemLease public model
```

Plugin architecture 未设计，所以新的 `SystemRegistration` **不增加** plugin/code-lifetime surface。

---

## 3. `SystemRegistry` semantic reset

旧 `SystemRegistry` concrete instance owner 语义必须移除。

新 `SystemRegistry` 只回答：

```text
“SystemTypeId X 当前 Product 知道吗？”
“如果知道，哪个 concrete/generated install thunk 构造它？”
```

它不回答：

```text
“这个 Simulation 里有哪些 live System instances？”
```

runtime instance ownership 完全移到 `Simulation` private implementation。

---

## 4. Exact `SystemRegistration`

冻结：

```cpp
class SimulationBuilder;

using InstallSystemFn =
    lux::cxx::expected<void, SystemBuildFailure> (*)(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept;

struct SystemRegistration final
{
    SystemTypeId type;
    std::uint32_t version{};
    InstallSystemFn install{};
};
```

不要追加：

```text
void* user_context
service locator
scheduler pointer
Registry pointer
Scene pointer
Asset client
Process client
Render client
code_lifetime
```

所有 construction inputs 只能来自：

```text
SimulationSystemView
SimulationBuilder explicitly permitted methods
concrete System/private package code
Registry::ctx() for shared L1 runtime mechanisms
```

---

## 5. Exact `SystemRegistry` surface

冻结：

```cpp
class SystemRegistry final
{
public:
    [[nodiscard]] lux::cxx::expected<void, SystemRegistrationFailure>
    add(SystemRegistration registration) noexcept;

    [[nodiscard]] lux::cxx::expected<void, SystemRegistrationFailure>
    add(std::span<const SystemRegistration> registrations) noexcept;

    [[nodiscard]] const SystemRegistration*
    find(const SystemTypeId& type) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
};
```

规则：

```text
no singleton requirement
Product explicitly owns/composes it
add only during cold composition
find read-only during Simulation build/execute
duplicate canonical type -> failure
hash collision -> compare canonical name and reject mismatch
add(span) all-or-nothing
```

不加：

```text
remove()
replace()
hot reload
plugin unload
observer callback
```

---

## 6. Registration source: explicit package contribution

每个 concrete System package 暴露一个静态 span function，例如：

```cpp
[[nodiscard]] std::span<const SystemRegistration>
transformSystemRegistrations() noexcept;
```

Product composition 显式：

```cpp
SystemRegistry systems;

LUX_TRY(systems.add(transformSystemRegistrations()));
LUX_TRY(systems.add(physicsSystemRegistrations()));
LUX_TRY(systems.add(spatial3dSystemRegistrations()));
```

禁止建立第二套：

```text
SystemRegistrar singleton
static linked-list System registry
SystemFactoryRegistry
ReflectionRegistry -> implicit auto-install path
```

Reflection/codegen 可以**生成 registration table 与 install thunk source**，但最终 composition 仍然是 `SystemRegistry::add(...)`。

---

## 7. `SimulationBuilder` exact permitted surface

第一版 public surface 只允许：

```cpp
class SimulationBuilder final
{
public:
    [[nodiscard]] ecs::Registry& registry() noexcept;

    template<System Type, class... Args>
    [[nodiscard]] lux::cxx::expected<Type*, SystemBuildFailure>
    emplaceSystem(
        SystemInstanceId instance,
        Args&&... args
    ) noexcept;

    template<System Type>
    [[nodiscard]] Type*
    findSystem(SystemInstanceId instance) noexcept;

    template<System Type, class Callable>
    [[nodiscard]] lux::cxx::expected<void, SystemBuildFailure>
    addSystemTask(
        SystemInstanceId instance,
        Callable&& callable
    ) noexcept;

    template<System Type, class Callable>
    [[nodiscard]] lux::cxx::expected<void, SystemBuildFailure>
    addSystemCommandTask(
        SystemInstanceId instance,
        ecs::EcsCommandProducerCapacity capacity,
        Callable&& callable
    ) noexcept;
};
```

**没有其他 public dependency bag。**

明确禁止：

```text
findService<T>()
service<T>()
context()
assetClient()
processClient()
renderClient()
scene()
worldStorage()
getAnythingByTypeToken()
```

`SimulationBuilder` 不是 DI container。

---

## 8. System-owned vs shared runtime mechanisms

System-specific runtime state：

```text
优先作为 concrete System / System::Impl 成员
```

多个 L1 Systems 真正共享的 runtime mechanism，如果其 lifetime 应等于 Registry：

```text
使用现有 EnTT Registry context (`registry.ctx()`) 存储具体 C++ type
```

例如某个 concrete package可以在 cold install 时：

```text
ensure HierarchyIndex/runtime helper in Registry context
construct System with direct reference to that concrete object
```

这条规则的目的就是**不新增**：

```text
SimulationServices
RuntimeResourceRegistry
ExternalResourceManager
```

`SystemAccessSpec::ExternalRead/ExternalWrite<T>` 继续只描述 TaskGraph hazard identity；它不是一个对象查找 API。

如果一个 runtime object 不适合放 Registry context，也不是某个 System 私有状态，则记录 architecture gap；不得现场创建 Services bag。

---

## 9. `emplaceSystem()` exact semantics

`emplaceSystem<Type>(instance, args...)`：

1. `instance` 必须属于当前 `SimulationDescription`；
2. 同一 instance 只能 emplace 一次；
3. concrete object 单独稳定分配，Simulation move 不得移动其地址；
4. private record 保存：instance、concrete TypeToken、object pointer、destroy thunk；
5. allocation/construction exception -> `SystemBuildFailure`；
6. build rollback 时按逆构造顺序销毁已创建 objects。

Public 不暴露 erased record。

`findSystem<Type>(id)`：

```text
只用于 cold install；返回当前已安装的 exact concrete type
```

wrong concrete type -> nullptr/build error at caller，不做 dynamic cast framework。

---

## 10. One-primary-task v1 rule

第一版：

```text
每个 System instance最多 1 个 primary scheduled TaskGraph task
```

允许 0-task System，例如只提供同步 query/helper 的 System。

但是：

```text
任何出现在 SimulationDescription execution dependency edge 中的 System
必须拥有 primary task
```

需要一个 System 拆成 N 个独立 TaskGraph nodes 时，先留到 architecture probes；**不要**提前引入 execution-point graph builder。

---

## 11. L0 dynamic dependency prerequisite

Phase 1 已为 `lux::task::TaskGraphBuilder` 增加：

```cpp
TaskDependencies
task::dependencies(span<TaskHandle>)
```

因此 `SimulationBuilder` private implementation 可以直接把动态 predecessor handles 作为现有 TaskGraph property 传入。

禁止在 L1 新增：

```text
SystemTaskRegistry
DependencyAdapter
TaskHandleService
```

---

## 12. `addSystemTask<Type>()` exact semantics

Callable contract：

```text
nothrow callable
invocable as Callable(Type&)
return type = void OR bool
```

Builder private implementation：

1. 找到已 emplace 的 `Type*`；
2. 收集当前 System 在 `SimulationDescription` 中所有 predecessor SystemInstanceId；
3. 从 private per-description-ordinal table 取得 predecessor primary `TaskHandle`；
4. 使用 Phase 1 `task::dependencies(...)`；
5. 使用现有：

```cpp
simulation::ecs::systemTaskResources<Type>()
```

生成 resource hazards；
6. 向 private `lux::task::TaskGraphBuilder` 添加一个 task；
7. 保存 returned `TaskHandle` 到当前 System ordinal；
8. duplicate task registration -> build failure。

`Callable(Type&) -> bool` 返回 `false` 时：

```text
private first-failure slot records current SystemInstanceId
```

该 slot 必须能被并行 TaskGraph workers 安全 first-wins 写入；baseline 使用 `std::atomic<std::uint64_t>` 保存 `SystemInstanceId::value`（0 = no failure）。不得为了失败汇聚创建 mutex/FailureManager。

`void` 表示该 task 没有 generic failure channel。

不要引入 templated error type erasure。Concrete System 如返回 `expected<void,E>`，install thunk 明确转换：

```cpp
return builder.addSystemTask<MySystem>(
    description.instanceId(),
    [](MySystem& system) noexcept -> bool {
        return static_cast<bool>(system.update());
    }
);
```

domain-specific error detail 仍由 concrete System 自己保存/log/diagnose；generic Simulation 只保证报告失败的 System instance。

---

## 13. `addSystemCommandTask<Type>()` exact semantics

用于现有 `EcsCommandWriter/EcsCommandBuffer` 路径。

Callable contract：

```text
nothrow callable
invocable as Callable(Type&, ecs::EcsCommandWriter&)
return type = void OR bool
```

`capacity` 必须由 concrete System configuration / Product-authored Simulation data 明确给出；禁止在 SimulationBuilder 内硬编码通用默认 command budget。

Builder cold path：

```text
assign one producer ordinal
append EcsCommandProducerCapacity to private capacity array
add one primary System task with same Type::Access/dependencies
```

Runtime task wrapper：

```text
commands.begin(producer ordinal)
    failure -> record current System task failure
invoke concrete callable(system, writer)
    bool false -> record current System task failure
writer destructor closes producer
```

如果 first-failure 已存在，后续 task不得覆盖 failure identity。

---

## 14. EcsCommandBuffer ownership and flush

如果至少存在一个 command task：

```text
Simulation::Impl owns exactly one EcsCommandBuffer
```

Builder finalize：

1. 用 collected `EcsCommandProducerCapacity[]` 调一次 `prepare()`；
2. 在所有 primary System tasks 后增加**一个** command flush task；
3. flush task使用现有：

```cpp
simulation::ecs::ecsCommandFlushTaskResources()
```

以及：

```cpp
task::dependencies(all_primary_task_handles)
```

4. flush affinity = `CALLER_THREAD`；
5. 若某 System task 已报告失败：`discardPending()`，不 apply；
6. 否则调用 `applyEcsCommands(registry, commands)`；failure 进入 private execution failure slot。

不允许每个 System 建自己的 generic command buffer manager。

0 个 command task时：

```text
Simulation不需要准备 command buffer，也不添加 flush task
```

---

## 15. Simulation dependency graph mapping

`SimulationDescription` 已有 `before_system -> after_system` dependency。

构造顺序固定：

1. validate every `SystemTypeId` registration；
2. v1 version rule：`SimulationSystemView::version() == SystemRegistration::version`；
3. derive deterministic topological order；
4. 按此顺序执行 install thunk；
5. 每个 primary task通过 private predecessor-handle table把同一 System dependency 映射成 TaskGraph explicit edge；
6. TaskGraph 自己继续增加 `SystemAccessSpec` resource hazard edges；
7. build immutable TaskGraph。

Tie-breaker：

```text
使用现有 stable SystemInstanceId ordering
```

不得依赖：

```text
registration insertion order
link order
unordered_map iteration
pointer order
```

### 15.1 Constructor dependency rule

Install thunk如果要 `findSystem<T>(other)`：

```text
other 必须是当前 System 的 declared predecessor
```

v1 将该 dependency 同时视为：

```text
cold construction order
runtime primary-task execution order
```

如果未来真实 probe 证明需要“只构造依赖、不产生执行 edge”的关系，再单独设计；当前禁止新增第二种 dependency graph。

---

## 16. Example install thunk

下面示例强调**形状**，不要求所有 System 有相同 update API：

```cpp
lux::cxx::expected<void, SystemBuildFailure>
installMySystem(
    SimulationBuilder& builder,
    SimulationSystemView description
) noexcept
{
    auto config = decodeMyConfig(description);
    if (!config)
        return lux::cxx::unexpected(...);

    // Shared L1 runtime mechanism may live in Registry context;
    // System-private runtime stays in MySystem::Impl.
    auto system = builder.emplaceSystem<MySystem>(
        description.instanceId(),
        builder.registry(),
        *config
    );
    if (!system)
        return lux::cxx::unexpected(system.error());

    return builder.addSystemTask<MySystem>(
        description.instanceId(),
        [](MySystem& value) noexcept -> bool {
            return value.runOneScheduledInvocation();
        }
    );
}
```

禁止 install thunk：

```text
construct Process/Render services
start async workflow
query global singleton
create thread
create scheduler
```

它是 cold synchronous construction code。

---

## 17. Simulation private representation

建议 private `Simulation::Impl` 持有：

```text
non-owning Registry*
shared_ptr<const SimulationDescription>
private erased stable System object records
private SystemInstanceId -> record ordinal lookup
private primary TaskHandle table during build (discard after graph build if not needed)
TaskGraph
optional/prepared EcsCommandBuffer
private first-failure state (`atomic<uint64_t>` for System task failure)
```

这些都不是 public types。

Runtime topology build 后 immutable：

```text
no runtime add/remove System in v1
```

因此删除旧 `SystemLease<T>` dynamic erase requirement；不要创建 compatibility facade。

---

## 18. `Simulation` exact public baseline

```cpp
class Simulation final
{
public:
    [[nodiscard]] static
    lux::cxx::expected<Simulation, SystemBuildFailure>
    create(
        ecs::Registry& registry,
        std::shared_ptr<const SimulationDescription> description,
        const SystemRegistry& system_types
    ) noexcept;

    [[nodiscard]] const SimulationDescription&
    description() const noexcept;

    [[nodiscard]] lux::cxx::expected<void, SimulationExecutionFailure>
    execute(task::TaskExecutor& executor) noexcept;
};
```

不重复把 Registry 传给 `execute()`；`Simulation` 在 create 时借用稳定 Scene-owned Registry。

`SimulationExecutionFailure` 最少区分：

```text
TASK_EXECUTOR_FAILURE
SYSTEM_TASK_FAILURE
ECS_COMMAND_FAILURE
```

并能携带：

```text
SystemInstanceId for SYSTEM_TASK_FAILURE
TaskExecutorFailure for TASK_EXECUTOR_FAILURE
EcsCommandFailure for ECS_COMMAND_FAILURE
```

不要求 type-erased concrete System error payload。

---

## 19. `execute()` exact v1 behavior

每次 `execute()`：

1. 清空 private first-failure state；
2. reset/reuse prepared `EcsCommandBuffer` if present；
3. 同步调用 existing `TaskExecutor::execute(graph)`；
4. TaskExecutor failure -> `TASK_EXECUTOR_FAILURE`；
5. graph complete 后读取 first-failure；
6. System false -> `SYSTEM_TASK_FAILURE`；
7. flush apply failure -> `ECS_COMMAND_FAILURE`；
8. success -> `{}`。

`execute()` 同步完成；L1 不创建 execution thread。

本 Phase 不承诺 generic step rollback：

```text
System direct component writes may already have occurred before failure
```

需要 deterministic transaction/replay semantics 时由后续 evidence 设计，不在这里造 command journal。

---

## 20. Thread/lane construction invariant

`LuxObject` constructor captures current thread affinity。

因此：

```text
Simulation::create()
```

必须由 Product 在 intended **Simulation Lane** 调用。

L1：

```text
does not create thread
does not dispatch itself to lane
does not own scheduler
```

单线程 Product：

```text
Simulation Lane == caller/Main lane
```

多-lane Product：Scene final construction和所有 LuxObject Systems construction都在 Simulation Lane 发生。

---

## 21. Scene/Registry ownership boundary

Simulation借用 Scene-owned Registry。

必须满足：

```text
Registry address stable for entire Simulation lifetime
Simulation destroyed before Registry
```

Shared runtime mechanism in `Registry::ctx()`也因此在 System destruction时仍然存在。

Simulation 不拥有 Registry。

---

## 22. L3 Systems

Concrete System 可以属于 L3：

```text
scene/spatial3d/...System
scene/spatial2d/...System
```

L1 Simulation 不 include这些 concrete headers。

L3 package提供自己的：

```text
SystemRegistration span
```

Product将其加入同一个 `SystemRegistry`。

禁止 L1：

```text
switch(SystemTypeId) on L3 names
hardcoded Spatial3D factory
```

---

## 23. Reflection/codegen role

Reflection/codegen 可以提供：

```text
canonical system type metadata
generated config decoder
generated SystemRegistration/install thunk source
```

runtime install最终只调用：

```text
SystemRegistration::install
```

不要求：

```text
RefClass::construct
RuntimeObject
per-step reflection
```

---

## 24. Explicit design hold: time/tick/phase

Phase 5 **禁止新增**：

```text
TimeDomainId
TickGroup
ScenePhase
SimulationClock
ClockManager
RateController
FrameInfo compatibility alias
```

`Simulation::execute()`当前只表示：

> 执行一次当前 compiled graph invocation。

这个 invocation对应多少 logical time，由 architecture probes 的 concrete Product loop决定。

Robot/Pixel/Physics probes先直接传 concrete dt 给各自 System-owned/configured state；Barrier B后再决定是否存在真正通用 TimeDomain/TickGroup API。

---

## 25. Tests

必须覆盖：

### SystemRegistry

```text
add/find
span add atomicity
duplicate type rejection
hash collision canonical-name rejection
no live System ownership
```

### Build

```text
unknown SystemTypeId
version mismatch
deterministic topological order
one primary task limit
0-task dependency rejection
undeclared constructor dependency rejection
build rollback destroys already-built Systems
L3 registration compiles without L1->L3 include
```

### Task mapping

```text
N predecessor Systems -> TaskDependencies -> TaskGraph explicit edges
Type::Access -> existing systemTaskResources
resource hazards remain active
no SystemTaskRegistry
```

### Command path

```text
command producer capacities collected exactly
one EcsCommandBuffer per Simulation
one final flush task
system false -> discard pending commands
applyEcsCommands failure -> execution failure
0 command tasks -> no command buffer/flush
```

### Lifetime/thread

```text
Simulation move preserves concrete object addresses
TaskGraph destroyed before System objects
System objects destroyed before Registry/context
LuxObject System created/executed on Simulation Lane
wrong-thread existing contract still catches misuse
```

---

## 26. Exit gate

Phase 5完成后，上层只依赖：

```text
SystemRegistry type catalog
Simulation::create
Simulation::execute
Registry ownership/lane contract
```

如果 L3 施工方发现自己需要新增：

```text
SimulationContext
SimulationServices
SystemTaskRegistry
SystemFactory
RuntimeResourceRegistry
```

必须停止并回到 Phase 5设计审查，不能现场补 adapter。
