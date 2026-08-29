# L0 TaskGraph 动态依赖列表前置实施规范（Baseline v3.1）

> 状态：**Normative prerequisite**。
>
> 施工 Phase：**1**。
>
> 影响：现有 `modules/core/task`；**不得新增 target/package**。
>
> 目的只有一个：让上层在冷构建期把一个动态数量的、已经存在的 `TaskHandle` 依赖传给现有 `TaskGraphBuilder`，而不是在 L1 再发明 `SystemTaskRegistry/TaskAdapter`。

---

## 1. 为什么这是 L0 前置，而不是 L1 workaround

现有 `TaskGraphBuilder` 已有：

```cpp
struct TaskResources final
{
    std::vector<TaskResourceAccess> values;
};
```

以及单个：

```cpp
struct TaskDependency final
{
    TaskHandle task{};
};
```

`SimulationDescription` 的 System dependency 数量是运行时/资产决定的，因此一个 System 可能有 0..N 个 predecessor。

错误实现方式：

```text
L1 SystemTaskRegistry
L1 DependencyAdapter
L1 temporary TaskHandle map exposed as service
```

真正缺失的是 generic TaskGraph 本身对“动态 dependency property list”的一个小型 owning convenience，与 `TaskResources` 完全对称。

---

## 2. Public Type Budget

本 Phase **唯一允许新增**的 public production type：

```text
TaskDependencies
```

允许增加与现有 free-function style 一致的：

```text
task::dependencies(...)
```

不允许新增：

```text
TaskRegistry
TaskDependencyRegistry
TaskDependencyBuilder
TaskGraphAdapter
TaskGraphContext
TaskScheduleManager
```

---

## 3. Exact public surface

修改现有 `lux/engine/task/Task.hpp`：

```cpp
namespace lux::task
{
    struct TaskDependencies final
    {
        std::vector<TaskHandle> values;
    };

    [[nodiscard]] inline TaskDependencies
    dependencies(std::span<const TaskHandle> values)
    {
        return TaskDependencies{
            std::vector<TaskHandle>(values.begin(), values.end())
        };
    }

    template<class Range>
        requires requires(const Range& value) {
            std::span<const TaskHandle>(value);
        }
    [[nodiscard]] TaskDependencies dependencies(const Range& values)
    {
        return dependencies(std::span<const TaskHandle>(values));
    }
}
```

`TaskDependencies` 与 `task::dependencies(...)` 名称已冻结；只允许格式化风格调整。

不提供：

```text
initializer-list-only second API family
mutable addDependency method
runtime graph mutation
```

---

## 4. TaskGraphBuilder integration

`TaskDependencies` 必须成为现有 property pack 的合法 property，和 `TaskResources` 同级。

在现有：

```cpp
detail::kTaskProperty
```

中加入 `TaskDependencies`。

增加一个 private collector：

```cpp
static void collectProperty(
    PendingTask& pending,
    TaskDependencies property
)
{
    pending.dependencies.insert(
        pending.dependencies.end(),
        std::make_move_iterator(property.values.begin()),
        std::make_move_iterator(property.values.end())
    );
}
```

最终验证继续只走现有 `addPending()`：

```text
invalid TaskHandle        -> INVALID_TASK
foreign builder handle    -> INVALID_TASK
forward dependency        -> DEPENDENCY_MUST_PRECEDE_TASK
duplicate dependency      -> DUPLICATE_DEPENDENCY
allocation failure        -> ALLOCATION_FAILURE
```

**不得因为新增 aggregate property 建第二套 dependency validation。**

---

## 5. Ordering and ownership

`TaskDependencies`：

```text
cold-path owning property
只在 TaskGraphBuilder::add() 调用期间消费
不进入 runtime TaskGraph public API
不改变 TaskHandle lifetime/meaning
```

`TaskGraphBuilder` 仍然保持：

```text
single-pass construction
explicit dependency only backward-reference
insertion order is valid topological order
resource hazards remain the existing second source of graph edges
```

---

## 6. Performance contract

这是冷路径 API。

允许 `TaskDependencies` 构造时一次 `std::vector` allocation；不要求 SBO/arena。

禁止为了避免这次冷路径 allocation 新增：

```text
SmallTaskDependencyVector
DependencyArena
DependencyScratch
TaskGraphBuildContext
```

如果未来 profile 证明 TaskGraph cold build allocation 成为问题，再单独优化现有 TaskGraph builder。

---

## 7. Tests

必须增加到现有 `modules/core/task` tests：

1. empty `TaskDependencies` 可用；
2. 1 个 dependency 与单独 `task::dependsOn()` 等价；
3. N 个 dependencies 全部形成 edge；
4. duplicate across `TaskDependencies` 内部 -> `DUPLICATE_DEPENDENCY`；
5. duplicate across `task::dependsOn(x)` + `task::dependencies({x})` -> 同样失败；
6. foreign-builder handle -> `INVALID_TASK`；
7. forward/non-preceding handle -> existing error；
8. resource hazard 行为完全不变；
9. existing installed consumers 不需要 adapter。

---

## 8. Exit gate

Phase 1 完成后必须满足：

```text
L1 可以直接把 vector/span<TaskHandle> 变成一个现有 TaskGraph property
```

并且仓库中不存在为这个需求新增的：

```text
SystemTaskRegistry
TaskDependencyAdapter
TaskGraphService
```

完成后才能进入 World/Simulation 等上层施工。
