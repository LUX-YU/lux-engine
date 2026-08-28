# Lux Process Execution 规范架构

状态：**Normative Design Candidate**

## 1. 目的

Process 不再被定义成“一个拥有所有异步功能的大 Runtime”。

新的定义：

> **Process = C++ Execution Model 上的引擎级异步基础设施。**

它不定义业务工作流，只允许业务模块用统一的 Sender/Receiver algebra 描述工作流。

```text
Domain-defined workflow
        │
        ▼
      Sender
        │
 ┌──────┼───────────┐
 │      │           │
 ▼      ▼           ▼
CPU    IO         Render/GPU
sched  sender       sender
 │      │           │
 └──────┼───────────┘
        ▼
 completion channels
 value / error / stopped
```

## 2. 五个 canonical C++ execution 概念

Lux 不重新包装：

```text
Sender
Receiver
Scheduler
Operation State
Scope / stop environment
```

用户应直接使用标准/stdexec：

```cpp
stdexec::then
stdexec::let_value
stdexec::starts_on
stdexec::continues_on
stdexec::when_all
stdexec::bulk
stdexec::get_parallel_scheduler
stdexec::spawn
stdexec::spawn_future
exec::async_scope
```

Process 不创建：

```text
ProcessSender
ProcessReceiver
ProcessScheduler
ProcessScope
ProcessTaskGraph
ProcessFuture
```

## 3. 完成通道

canonical completion channel：

```text
set_value(...)   成功
set_error(E)     typed semantic/runtime failure
set_stopped()    cancellation
```

禁止把正常异步错误重新压成：

```cpp
set_value(expected<T, E>)
```

除非某个 lower-layer ABI/capability 已经以 expected 为固定 wire contract；在 Process
adapter 边界应尽可能重新投影为 typed error channel。

C++ exception 只允许：

- allocation boundary；
- foreign library boundary；
- standard/library operation 本身可能抛出的 construction/connect boundary。

业务 semantic error 不通过 throw。


## 3.1 noexcept boundary

`start()` 和 completion 必须 `noexcept`。

`connect()`/operation-state construction 是冷 construction boundary，不应由 Lux 无条件强制 `noexcept`。
如果 Receiver move、foreign sender connection 或 allocation 在该边界抛出，允许由调用方/标准 execution
consumer 在边界处理；禁止把这种 construction exception 当作日常 domain semantic error。

## 4. Process 不拥有 CPU universe

Process 不创建自己的“background CPU thread pool”。

CPU-heavy 工作：

```cpp
auto cpu = stdexec::get_parallel_scheduler();
```

或由 Host/Product 注入等价 scheduler。

目标：

```text
                  shared parallel execution resource
                         /        |        \
                        /         |         \
              Simulation      Physics     Process CPU
```

不要：

```text
TaskExecutor pool
+ Process TBB pool
+ Physics pool
```

各自按 hardware_concurrency() 抢核。

## 5. Blocking execution 是例外

无法异步化的 blocking API 可以拥有小型专用 blocking resource。

它不是 general CPU pool。

本参考实现的 FileIo 使用小型 blocking worker backend，原因仅是：

- portable；
- 可测试；
- public API 与未来 IOCP/io_uring backend 完全一致。

Production Windows/Linux backend 可以替换内部实现而不改变 `FileClient::read()` sender。

## 6. Main Thread 不属于 Process

Host owns main thread。

Host 提供一个满足 scheduler concept 的 lightweight handle。

Process 不创建：

```text
MainThreadMailbox
MainThreadDispatcher
ProcessMainThread
```

如果 Host 已经有 Object/UI main queue，main scheduler 应建立在该 owner 上。

## 7. Render 不属于 Process

Render owns:

- Render thread；
- frame ingress；
- resource upload ingress；
- Vulkan queue ownership；
- staging；
- timeline；
- GPU completion。

Process 只要求 Render 对外提供 Sender：

```cpp
renderer.upload_texture(texture)
renderer.upload_mesh(mesh)
```

上层不应看到：

```text
RenderUploadSession
callback registry
request id allocation
SPSC reply pumping
Vulkan objects
```

内部仍可以使用最优 transport：

- frame lane: SPSC；
- resource ingress: bounded MPSC；
- transfer lane: SPSC；
- GPU: Vulkan timeline semaphore。

**统一 execution 语义 != 强制统一底层 queue。**

## 8. IO 是 Sender，不是“IO thread”

真正 native async IO：

```cpp
files.read(path)
```

本身就是 Sender。

只有 fallback blocking API 才表达为“blocking execution resource 上的同步调用”。

API 不要求调用者：

```cpp
continues_on(io_thread)
then(read_file)
```

调用者只处理数据依赖：

```cpp
files.read(path)
| continues_on(cpu)
| then(decode)
```

## 9. Operation State 是 transport lifetime anchor

跨线程异步 sender 的 operation state 地址在 `start()` 后必须稳定。

因此可以把 operation state 本身作为 ingress node：

```text
connect(sender, receiver)
        │
        ▼
  Operation State
        │ start
        ▼
 bounded ingress stores pointer
        │
        ▼
 consumer / OS / GPU
        │
        ▼
 set_value/error/stopped
```

这允许移除：

- per-request callback registry；
- per-request `std::function`；
- request-id→callback global map；
-额外 Future/Promise object。

底层 queue 不拥有 operation state；Scope/consumer owns lifetime。

## 10. Package purity

```text
engine/process/
  CMakeLists.txt
  execution/
  io/
```

Parent `process/`：

> Collection only.

`execution/`、`io/`：

> Leaf packages.

不允许 parent root 放 production include/src。

## 11. Dependency direction

```text
CORE async_port
      ▲
      │
PROCESS execution
      ▲
      │
PROCESS io

Render / Scene / Streaming / Asset runtime clients
      └──────────────► PROCESS execution/io
```

Process 不依赖：

```text
Scene
Editor
Authoring
Toolchain
Host
Render concrete backend
Asset workflow
```

Process 可以依赖：

```text
Platform
Core
Resource mechanism if truly generic
Process sibling lower leaf
```

## 12. Explicit non-goals

Process v1 不解决：

- gameplay parallelism；
- Script parallelism；
- Simulation frame DAG；
- Physics scheduling；
- Render graph；
- model/material loading policy；
- asset residency；
- Scene lifecycle；
- coroutine error facade；
- universal workflow graph。

这些模块只消费 Process execution substrate。
