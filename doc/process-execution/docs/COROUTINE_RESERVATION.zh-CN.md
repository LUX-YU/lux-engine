# Coroutine Reservation

## 1. 冻结原则

> Coroutine 是 Sender graph 的顺序语法，不是第二套异步 Runtime。

Process v1 不创建：

```text
ProcessTask<T>
CoroutineRuntime
CoroutineScheduler
CoroutineManager
```

所有 Process primitive 首先必须是合法 Sender。

于是未来：

```cpp
auto bytes = co_await files.read(path);
auto gpu   = co_await renderer.uploadTexture(texture);
```

不会要求修改 IO/Render backend。

## 2. Thread reuse 与 coroutine 是不同问题

Scheduler/resource 决定：

- 哪些 worker/thread 被复用；
- work 在哪里运行。

Coroutine 决定：

- suspend/resume control flow；
- coroutine frame lifetime；
- 顺序代码外观。

Coroutine 本身不创建 worker pool。

## 3. Affinity 必须显式

Sender：

```cpp
files.read(path)
| continues_on(cpu)
| then(decode)
| let_value(upload)
| continues_on(main);
```

Coroutine 未来等价：

```cpp
auto bytes = co_await files.read(path);

co_await resume_on(cpu);
auto decoded = decode(bytes);

auto handle = co_await renderer.uploadTexture(decoded);

co_await resume_on(main);
install(handle);
```

`co_await files.read()` 后不能默认假设已经在 CPU business scheduler。

## 4. Error model 是 coroutine façade 的冻结前置条件

Lux semantic errors 不使用 C++ exceptions。

因此不能简单冻结一个 task，使：

```text
set_error(E)
```

自动变成：

```cpp
throw E;
```

v1 保持 Sender typed error channel。

Coroutine façade 只有在确定 non-throwing typed error propagation 后才冻结。

可选未来形式：

```cpp
auto result = co_await as_result(sender);
// result = expected<T, E>
```

但这只能是 coroutine adapter，不应反过来污染 canonical Sender 的 error channel。

## 5. Allocation

Coroutine frame 可能分配。

因此 coroutine 不适合：

- per entity callback；
- per Script invocation；
- frame microtask；
- Hook/Event hot dispatch。

适合：

- asset/scene streaming；
- network/session workflow；
- toolchain/editor long-latency operations；
- coarse async resource loading。

## 6. Migration rule

只有在以下条件全部满足后引入 coroutine public surface：

1. stdexec/std::execution task semantics stabilized for project toolchain；
2. typed non-throwing error policy frozen；
3. allocator/frame storage benchmark completed；
4. scheduler-affinity behavior documented；
5. cancellation maps exactly to stopped；
6. no second runtime/pool/event loop。
