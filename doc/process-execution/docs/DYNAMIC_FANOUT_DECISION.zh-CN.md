# Dynamic Runtime-Cardinality Fan-out Decision

## 结论

Process v1 **不冻结** 一个自定义 `parallelTransform` public API。

这不是缺功能，而是主动避免过早发明第二套 async graph。

## 已有标准能力

编译期已知 N：

```cpp
stdexec::when_all(a(), b(), c())
```

纯 CPU runtime index space：

```cpp
sender
| stdexec::bulk(stdexec::par, count, fn)
```

结构化动态启动：

```text
async scope
spawn
spawn_future
```

## 尚未被我们完全满足的 case

```text
runtime range<Item>
    ↓
Fn(Item) -> asynchronous Sender<Result, Error, Stopped>
    ↓
bounded concurrency C
    ↓
ordered vector<Result>
```

Model dependencies 就属于这一类。

## 为什么现在不自己造

一个真正可冻结的版本至少要同时正确解决：

- child sender completion-signature deduction；
- multiple typed error channels normalization；
- stopped propagation；
- parent stop → all active child stop；
- first error 与 simultaneous error race；
- active child operation-state lifetime；
- bounded C；
- N actual input/output O(N) memory；
- 不允许 N 次 general heap allocation；
- completion exactly once；
- shutdown while children are in foreign IO/GPU backends；
- output order；
- zero unrelated global scan。

如果只是：

```text
vector<future>
shared_ptr<BatchJoin>
remaining--
```

它能工作，但不配成为 frozen Process public API。

## Wave-5 acceptance shape

未来若标准/stdexec sequence facilities仍不能直接表达该 case，只增加**一个** generic algorithm。

候选语义（非冻结 API）：

```cpp
bounded_async_transform(
    range,
    max_concurrency,
    fn,
    error_policy,
    order_policy)
```

但名字、参数和具体 sender type在 Wave 5 benchmark 前都不冻结。

## Implementation invariant

推荐最终 implementation：

```text
one outer operation-state allocation/arena
    ├── N result slots
    ├── N child op-state slots or O(C) safely recycled slots
    └── C active children maximum
```

不接受：

```text
N × shared_ptr
N × promise/future
N × std::function
N × generic heap operation state
global task registry
```

## 业务在 Wave 1~4 如何处理

小固定 fan-out：

```cpp
when_all(...)
```

纯 CPU collection transform：

```cpp
bulk(par, ...)
```

需要 runtime async N 的第一个真实 consumer（Model/Material/Streaming）应和 Wave 5 一起提交，
用真实 workload 驱动最终 algorithm，而不是 Process 先猜 API。
