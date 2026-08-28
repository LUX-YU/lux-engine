# 主循环、Process 与 Render Sender

## 1. Product composition

概念伪代码：

```cpp
int main()
{
    Host host;

    auto main = host.mainScheduler();
    auto cpu  = stdexec::get_parallel_scheduler();

    auto timers = lux::process::TimerQueue::create({8192}).value();
    auto file_io = lux::process::io::FileIo::create({
        .blocking_worker_count = 2,
        .queue_capacity = 2048
    }).value();

    Renderer renderer{/* owns render domain */};

    AssetLoader assets{
        file_io.client(),
        timers.client(),
        cpu,
        main,
        renderer.client()
    };

    Scene scene{
        assets.client(),
        renderer.client()
    };

    run(host, scene, renderer, cpu);
}
```

没有 process-wide Services object。

每个 consumer 显式持有所需 capability。

---

## 2. Main loop

```cpp
void run(...)
{
    while (!host.shouldQuit())
    {
        host.pollPlatformEvents();

        // Drain only continuations whose affinity explicitly targets Main.
        host.drainMainScheduler();

        simulation.beginFrame();

        // Initial policy: gameplay/script serial.
        simulation.runGameplay();
        simulation.runScripts();

        // Frame-local parallel DAG. This is NOT Process.
        simulation.executeFrameTasks(cpu);

        simulation.flushCommands();

        auto frame = renderer.extractFrame(simulation);
        renderer.submitFrame(std::move(frame));

        host.finishFrame();
    }
}
```

主循环没有：

```cpp
if (model_loaded)
if (texture_uploaded)
poll_async_runtime()
pump_asset_callbacks()
```

这些全部由 sender graph completion 驱动。

---

## 3. Texture workflow

```cpp
auto TextureLoader::load(TextureLocation location)
{
    return files_.readRange(
               location.file,
               location.offset,
               location.byte_size,
               {.max_bytes = location.byte_size})
        | stdexec::continues_on(cpu_)
        | stdexec::then([](FileBytes bytes) noexcept {
              return decode_texture(std::move(bytes));
          })
        | stdexec::let_value([render = render_](DecodedTexture texture) mutable {
              return render.uploadTexture(std::move(texture));
          });
}
```

注意没有：

```cpp
continues_on(render_scheduler)
```

Render sender 自己拥有进入 Render execution domain 的语义。

---

## 4. Render upload sender

推荐 public contract：

```cpp
class RenderClient
{
public:
    [[nodiscard]]
    UploadTextureSender uploadTexture(DecodedTexture texture) const noexcept;

    [[nodiscard]]
    UploadMeshSender uploadMesh(DecodedMesh mesh) const noexcept;
};
```

`UploadTextureSender` 的 operation state 可以直接作为 ingress node：

```text
Operation State
  receiver
  owning texture payload
  intrusive queue metadata
  lifecycle state
```

`start()`：

```text
try bounded ingress
  fail -> set_error(QUEUE_FULL)
  accepted -> Render owns only a pointer until completion
```

Render 完成：

```text
GPU success -> set_value(TextureHandle)
GPU failure -> set_error(RenderError)
cancel      -> set_stopped()
```

不再需要 public callback registry。

---

## 5. Internal Render topology

建议保留不同 transport：

```text
Frame submission:
Main/producer
   ↓
SPSC frame lane
   ↓
Render Thread

Resource upload:
many workers
   ↓
bounded MPSC ingress
   ↓
Render Thread
   ↓
transfer path
   ↓
GPU timeline
```

为什么 resource upload 用 MPSC：

- producer 数量动态；
- CPU decode worker 可以直接 submit；
- 不需要先绕 Process coordinator 再转 SPSC。

为什么 frame lane 仍然 SPSC：

- producer 固定；
- 每 frame 一次；
- 连续数据；
- SPSC 更小、更快、更可预测。

---

## 6. Transfer thread 是否删除

Execution integration **不要求删除** transfer thread。

先隐藏，再 benchmark。

v1 可以继续：

```text
Render Thread
    ↓
Transfer Thread
    ↓
VkQueue / timeline
```

未来如果 staging/recording 更适合 shared parallel resource：

```text
Render owner
    ↓
CPU parallel scheduler
    ↓
record/staging result
    ↓
Render-owned queue submit
```

public Sender API 不变。

---

## 7. GPU Scheduler 不是默认抽象

不要因为 execution 支持 scheduler 就把 Vulkan queue 强行叫：

```text
GpuScheduler
```

普通 scheduler 的语义是“在该 execution resource 上继续执行 C++ continuation”。

Vulkan queue 不能执行任意 C++ lambda。

因此更自然：

```text
CPU execution resource -> Scheduler
GPU operation          -> Sender
```

如果未来有真正的 device-code execution domain（CUDA/HIP/compute DSL），那才可能是一个合法 GPU scheduler/domain customization。

---

## 8. Completion affinity

`renderer.uploadTexture()` 的 `set_value` 可能由 Render owner thread 发出。

业务如果要回 Main：

```cpp
renderer.uploadTexture(texture)
| stdexec::continues_on(main_)
| stdexec::then([this](TextureHandle h) noexcept {
      install(h);
  });
```

这比 callback registry 手工 post 回 Main 更统一。
