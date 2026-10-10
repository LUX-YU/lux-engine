# 05 — VulkanBackend、RenderRuntime、Frame、Scene 与 Target R2

## 1. 终态

```text
RenderRuntime
    ↓ owns backend thread/session
VulkanBackend
    ├── SceneRenderer
    ├── RenderTargetRegistry
    ├── FrameExecutor
    ├── VulkanFrameDriver
    └── Vulkan Foundation
```

不再存在：

```text
RuntimeServer : GeneralRenderServer
virtual tick hierarchy
historical UIRenderServer seam
```

## 2. RenderRuntime

职责：

```text
backend thread lifetime
Main-side transport endpoints
pump/progress
diagnostics
feature registration transaction host
stop / terminal failure
```

不拥有：

```text
Engine Scene/ECS
built-in Feature implementation
Scene-side resource domain
```

最小真实 API：

```cpp
class RenderRuntime final
{
public:
    static RenderResult<std::unique_ptr<RenderRuntime>>
    create(RenderRuntimeConfig) noexcept;

    RenderControl& control() noexcept;
    RenderUploadPort uploads() noexcept;

    RenderResult<EFrameSubmit>
    submit(RenderProgram&) noexcept;

    RenderResult<RenderPumpResult>
    pump(RenderPumpBudget) noexcept;

    RenderRuntimeStatus status() const noexcept;
};
```

R5 开始就建立真实最小 Runtime；禁止先造 `RenderRuntimeLikeHost` 再迁移。

## 3. VulkanBackend

`final` + composition。

职责：

```text
consume backend transport endpoints
own Vulkan session/device mechanisms
typed backend dispatch
own RenderScenes/Targets
execute frame
publish replies/diagnostics
```

不抽 `IRenderBackend`。

## 4. SceneRenderer

唯一 RenderScene membership authority：

```text
create/find/retire scene
scene frame progression
Feature registration/runtime composition access
```

旧 `Renderer` 顶层语义删除。

## 5. RenderScene

拥有：

```text
persistent SceneData providers/capabilities
FeatureInstance set
View set
scene-local GPU/resource state
graph definition/cache state
```

不拥有：

```text
RenderRuntime
VulkanDevice global owner
transport queues
ECS
```

## 6. SceneCapability table 只做 non-owning projection

Capability 的真实 owner 是 provider Feature/domain resource。

例：

```text
MeshDataProvider Feature
    owns MeshSceneData

SceneCapabilityIndex
    MeshSceneData capability
        → provider FeatureHandle / typed bind address
```

Capability index **不得拥有第二份 MeshSceneData**。

consumer attach：

```text
resolve once
store typed pointer/handle
```

execution 不再查 capability index。

## 7. View

View 是 backend Scene 内一等 lifetime unit：

```text
identity/state
extent
per-view backend storage
graph resource state
feature-owned per-view state lifecycle
```

Temporal history 未来可由 concrete Feature 持久拥有，但 V2 不实现 UniversalHistory registry。

## 8. Per-view state authority

不继承 V1：

```text
unordered_map<feature_index, unordered_set<view_index>>
```

作为既定方案。

Vertical slice 在：

```text
FeatureInstance-owned per-view dense slots
或
View-owned feature-state slots
```

中选一个最简单的 single authority。

选择依据：

```text
destruction semantics
cache locality
真实 Feature/View count benchmark
```

禁止双向 registry。

## 9. FrameExecutor

logical orchestration：

```text
target order
scene/view selection
scene-global passes
view-local passes
cross-target dependency
```

不管 fence/present native details。

## 10. VulkanFrameDriver

native frame：

```text
FIF slot
fence wait/reset
command buffers
acquire
queue submit
present
GPU completion watermark
```

0 Mesh/Shadow/PointCloud knowledge。

## 11. RenderFrameState / per-FIF scratch

单帧 mutable data：

```text
FrameStamp
command buffers
acquire/present facts
reference to persistent slot scratch
```

per-FIF scratch 复用：

```text
submission list
multi-queue descriptors
external waits/signals
presentation list
target/view batch
```

fence 证明 slot 可复用后 `clear()` 保留 capacity。

## 12. Target / Presentation

语义：

```text
RenderTargetRegistry
PresentationTarget
PresentationRetirement
VulkanSwapchain
OffscreenTarget/Pool
```

semantic target 存在即 usable；native backing 需要延迟释放时转交 private retirement owner，不制造 public zombie state。

## 13. Direct/protocol authority

同一 backend semantic op 只有一个 implementation：

```text
createScene
createView
createTarget
registerFeature
resource/control op
```

transport handler 调 authoritative operation。

test direct access 使用 internal TestAccess，不保留第二套 public facade。

## 14. Device Lost policy

`VK_ERROR_DEVICE_LOST`：

```text
VulkanBackend records terminal failure
RenderRuntime transitions to terminal failed/stopped state
stop accepting new semantic work
settle/publish terminal diagnostics as protocol allows
destroy/retire private native ownership
```

恢复：

```text
host constructs a NEW RenderRuntime
```

V2 **不实现**：

```text
RECREATING_DEVICE
RESTORING_SCENES
RESTORING_FEATURES
RESTORING_TARGETS
```

等自动恢复状态机。

## 15. 性能 hard gate

steady frame：

```text
0 first-party infrastructure heap allocations
0 string dispatch
0 RTTI
0 service locator
0 dynamic Feature/Data route lookup per entity/draw
0 lazy pipeline/layout creation
```

CPU p50/p95 相对同机 baseline 无解释 >5% regression 即 STOP。

## 16. 完成门禁

```text
0 GeneralRenderServer public surface
0 RuntimeServer
0 virtual server tick hierarchy
0 FrameRuntime
0 RendererThread
VulkanBackend 0 built-in Feature dependency
Device Lost terminal policy tested
clear/offscreen/present minimal vertical slice
shutdown/backpressure/device failure tests
```

## 17. R2：共享资源、Capability 指针与退休

跨 Scene 共用 GPU 资源不意味着 GPU backing 无条件永久驻留。Runtime shared resource 的最后 CPU semantic retain 释放，不得绕过 program packet retain、GPU in-flight frame reference 和 fence-proven retirement。`RenderResourceDomain` 只掌握 Scene-side 的请求/handle/receipt authority，不再拥有 backend native GPU backing 的第二份真相。

Provider Feature 移除前，应先解除/失效它提供的 SceneCapability、阻止新 consumer attach，处理已存在的 dependent Feature 生命周期，然后才释放 provider native resource。`RenderScene` 继续是 Feature membership/lifecycle single authority，Capability index 只是 non-owning 冷态投影。
