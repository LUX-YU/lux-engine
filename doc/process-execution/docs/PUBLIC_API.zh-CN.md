# Process Public API

## 1. Public surface target

第一版只增加：

```text
lux::process::TimerQueue
lux::process::TimerClient
lux::process::ETimerError

lux::process::portSender(...)

lux::process::io::FileIo
lux::process::io::FileClient
lux::process::io::FileError
lux::process::io::FileReadOptions
```

没有 Runtime/Manager/Context/Registry/Builder。

---

## 2. Timer

Owner：

```cpp
auto queue = TimerQueue::create({
    .capacity = 8192
});
```

Capability：

```cpp
TimerClient timer = queue->client();
```

Sender：

```cpp
auto s = timer.after(250ms);
```

Completion：

```text
set_value()
set_error(ETimerError)
set_stopped()
```

复杂度：

```text
register      O(log T)
cancel        O(log T)
fire          O(log T)
wake          O(1)
memory        O(capacity)
```

没有 per-timer queue allocation。

Timer queue 使用 operation-state pointer 作为 queued item。

---

## 3. File IO

Owner：

```cpp
auto file_io = io::FileIo::create({
    .blocking_worker_count = 2,
    .queue_capacity = 1024,
});
```

Capability：

```cpp
io::FileClient files = file_io->client();
```

Whole file：

```cpp
auto sender = files.read(
    path,
    {.max_bytes = 64_MiB});
```

Range：

```cpp
auto sender = files.readRange(
    path,
    offset,
    bytes,
    {.max_bytes = 8_MiB});
```

Completion：

```text
set_value(FileBytes)
set_error(FileError)
set_stopped()
```

`FileClient` 不承诺 completion affinity。

因此：

```cpp
files.read(path)
| stdexec::continues_on(cpu)
| stdexec::then(decode);
```

是正确写法。

调用者不得假设 read completion 已经处于 CPU business worker。

---

## 4. `OperationPort<T>` bridge

`OperationPort` 继续属于 L0 capability。

Process 只提供 adapter：

```cpp
auto sender = lux::process::portSender(
    port,
    Operation{...});
```

把：

```text
OperationOutcome<T> = expected<Value, OperationFailure<Error>>
```

投影成：

```text
set_value(Value)
set_error(OperationFailure<Error>)
set_stopped()
```

该 adapter 不创造 operation registry。

重要限制：

当前 L0 OperationPort 没有 post-admission cancellation primitive。

因此：

- start 前 stop → `set_stopped`；
- accepted 后是否可取消由 endpoint contract 决定。

真正需要强 cancellation 的 domain API 应直接实现自己的 Sender，而不是把所有东西硬塞进 OperationPort。

---

## 5. 用户自定义 Sender

这是 primary extension point。

```cpp
auto load_texture(VfsPath path)
{
    return files_.read(std::move(path))
        | stdexec::continues_on(cpu_)
        | stdexec::then([](FileBytes bytes) noexcept {
              return decode_texture(std::move(bytes));
          })
        | stdexec::let_value([this](DecodedTexture texture) {
              return renderer_.upload_texture(std::move(texture));
          })
        | stdexec::continues_on(main_);
}
```

Process 不需要知道：

```text
Texture
AssetId
Renderer
decode_texture
```

---

## 6. 用户自定义 Receiver

允许：

```cpp
struct Receiver
{
    using receiver_concept = stdexec::receiver_tag;

    void set_value(TextureHandle value) && noexcept;
    void set_error(TextureLoadError error) && noexcept;
    void set_stopped() && noexcept;

    auto get_env() const noexcept;
};
```

但 ordinary gameplay/business code 不建议显式 `connect/start` 后把 operation state 放局部变量。

正确 lifetime owner：

```text
async scope
session owner
scene owner
request owner
explicit integration owner
```

Operation state 在异步完成前必须存活。

---

## 7. 为什么不提供 Process Scope

因为标准模型已经有 async scope。

推荐：

```cpp
exec::async_scope scope;

stdexec::spawn(
    std::move(sender),
    scope.get_token());

...
stdexec::sync_wait(scope.join());
```

Process 再包一层只会增加 vocabulary 和 allocator/lifetime 差异。

---

## 8. 为什么不提供 Process CPU scheduler

C++26/system execution context 已经定义共享 parallel scheduler 方向：

```cpp
auto cpu = stdexec::get_parallel_scheduler();
```

Lux Product 可以替换/适配 backend，但 Process API 不应该暴露 TBB/TaskExecutor/std::thread identity。

---

## 9. Header / template policy

Sender/receiver `connect()` 必然 template-heavy，这是正确的。

规则：

> owner concrete, algorithm generic.

Concrete:

```text
TimerQueue
TimerClient
FileIo
FileClient
```

Generic:

```text
sender operation state
receiver connect
pipeline composition
portSender<T>
```

跨 DLL boundary 不导出 giant composed sender type。

公开 concrete API 应返回小型 named sender（如 `FileReadSender`），其 `connect<Receiver>()` 在 header 中实例化。


---

## 10. Owner lifetime / shutdown invariant

`TimerQueue` 和 `FileIo` 是 execution-resource owners。

Normative rule：

```text
owner lifetime
    > all accepted operations
```

Product shutdown 应：

```text
stop owning scopes
→ join scopes
→ requestStop resource owner
→ destroy resource owner
```

不要在该 resource 自己的 completion receiver 内析构 owner。Reference backend 的 worker/timer
thread 是 owner implementation detail；owner teardown 是 host/composition thread responsibility。

这不是业务 sender 的 thread-affinity requirement，而是 execution-resource 的 structured lifetime requirement。
