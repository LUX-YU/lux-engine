# Process 实施方案

## Entry state

Latest reviewed main:

```text
25992bb578ccc7765a0f238c582f62d5424352d1
```

Latest production closure:

```text
07453d479538a0cae4037ea18725390c715b4bda
```

L1 worker-event race has been closed with atomic writer tracking and deterministic multi-worker regression.
Process can proceed after the architecture Wave-0 gate lands.

---

# Wave 0 — Architecture entry

必须先于任何 Process production `.cpp`。

1. 创建：

```text
engine/process/
  CMakeLists.txt
  execution/
  io/
```

2. `engine/process` 必须是 collection only。

3. Root `engine/CMakeLists.txt` 在 runtime products 加：

```cmake
add_subdirectory(process)
```

4. `ValidateSourceArchitecture.cmake` 必须扫描：

```text
engine/process/*/include
engine/process/*/sinclude
engine/process/*/pinclude
engine/process/*/src
```

5. 禁止 Process include：

```text
scene
editor
authoring
toolchain
host
legacy
```

6. target DAG：

```text
LAYER PROCESS
PRODUCT RUNTIME
```

---

# Wave 1 — `process_execution`

只实现：

- Timer sender；
- OperationPort sender adapter；
- stdexec dependency/install consumer；
- no Runtime/Builder/Scope/Scheduler wrapper。

Tests：

- lazy: sender construction does not start；
- connect does not start；
- start exactly once；
- value/error/stopped exactly once；
- timer cancellation；
- timer queue capacity；
- shutdown with live timers；
- stale client after owner destruction；
- zero general heap allocation on steady timer schedule/cancel if scope allocator excludes op-state ownership。

---

# Wave 2 — `process_io`

Public contract first：

```text
FileIo owner
FileClient capability
read
readRange
```

Reference backend：

- small blocking pool；
- bounded preallocated ingress；
- operation state pointer transport；
- no callback registry；
- cooperative cancellation；
- whole/range read；
- typed FileError。

然后平台 backend：

Windows：

```text
CreateFileW
OVERLAPPED
IOCP / Asio native file
```

Linux：

```text
io_uring when available
fallback blocking
```

Important：

> platform backend change must not change public sender types or semantics.

---

# Wave 3 — Render sender bridge removal

不在 Process 中实现 Render。

改 Render public client：

```text
trySubmit -> Sender
RenderRequest -> Sender completion
callback registry -> operation state receiver
Process coordinator bridge -> remove
```

目标 topology：

```text
producer worker
    ↓
Render Sender operation state
    ↓
bounded MPSC render ingress
    ↓
Render thread
    ↓
transfer/GPU
    ↓
set_value/error/stopped
```

Frame SPSC 保留。

GPU internal pipeline first remains unchanged.

---

# Wave 4 — Asset/Streaming consumers

只在外部 module 定义：

```text
LoadModel sender
LoadTexture sender
LoadWorldSection sender
```

不新增 Process operation types。

典型：

```text
File Sender
→ CPU decode
→ runtime dependency fanout
→ Render Sender
→ Main adoption
```

---

# Wave 5 — Dynamic runtime-cardinality fanout

不要在第一批仓促发明新的 TaskGraph。

优先评估最新 stdexec sequence/bulk/spawn facilities。

如果仍缺：

```text
range<Item>
→ bounded concurrent async Fn(Item)->Sender
→ collect Results
```

则新增一个单一 generic algorithm。

Frozen requirements：

- bounded concurrency；
- O(N) state for N actual inputs/output；
- no global registry；
- stop propagated to active children；
- first-error policy and collect-all policy must be explicit；
- at most one downstream completion；
- no per-item general heap allocation after operation-state arena construction；
- preserve input order by default；
- no one-task-per-entity use。

该 algorithm 在通过 allocation/scaling/lifetime benchmark 前不进入 public frozen API。

---

# Wave 6 — Coroutine façade

最后做。

不是 Process migration blocker。

必须满足 `COROUTINE_RESERVATION.zh-CN.md` 的 gate。

---

# Wave 7 — Delete legacy slice

当以下行为已经覆盖：

- timer；
- structured lifetime；
- file IO；
- render sender integration；
- representative asset workflow；
- clean shutdown；

删除对应：

```text
legacy/engine/runtime/execution
```

不提供 compatibility target/header。
