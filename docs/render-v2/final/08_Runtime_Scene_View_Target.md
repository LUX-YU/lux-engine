# 08 — 真实 RenderRuntime、Backend、Scene/View/Target 与独立 Render Progress

## 8.1 终态只有一个实际执行域

`RenderRuntime` 是有明确单一拥有者的程序端对象，负责 Transport 前后端端口、唯一后台执行线程的建/关、wake/stop/terminal error、request/diagnostic，并只拥有一个 `VulkanBackend`。`VulkanBackend` 组合 R4 device/session/queue/retirement、SceneRenderer、Graph/Shader compiled caches、FrameExecutor、FrameDriver、Target owners。`RenderRuntime` 不认识 ECS/Material/Mesh，`VulkanBackend` 不知道 Engine Scene System 的源码类型。

不要复活 V1 `GeneralRenderServer/RenderServerFacade/RendererThread` 的多层 Server/Renderer，也不额外加 Editor-specific `UIRenderServer::tick`。Editor/Player/Headless/Offscreen 使用同一 Backend execution domain，按 Target 和 Feature 组合差别运行。

## 8.2 精确拥有关系与对外 API（概念性）

```cpp
struct RenderRuntimeOptions {
    RenderTransportCapacity lanes;
    RenderDeviceRequirements gpu;
    uint32_t frames_in_flight{2};
    // Pacing policy explicit: vsync/surface/offscreen demand,
    // NEVER a mandatory Simulation packet clock.
};

class RenderRuntime final {
public:
    [[nodiscard]] static RenderResult<std::unique_ptr<RenderRuntime>>
        create(RenderRuntimeOptions) noexcept;
    ~RenderRuntime() noexcept;
    RenderRuntime(const RenderRuntime&) = delete;
    RenderRuntime& operator=(const RenderRuntime&) = delete;
    // typed ports for single producer ownership;
    // request/completion/diagnostic observation and shutdown contract.
};
```

真实接口应落在已认可的 `RenderTransport`/runtime types 上，不为示例创造重复的 `RuntimeManager`, `RenderCoordinator` 或后台 TaskScope。`RenderView` 只持 View identity/generation、camera/view data reference、target association、extent/projection/effect settings、per-view history/feature state、可选 `RenderPathId`。**没有每 View 必有 `GraphInstance` 属性**。

## 8.3 Scene Renderer 与持久数据

`SceneRenderer` 是 `RenderSceneId → RenderScene` membership 的唯一 authority。`RenderScene` 拥有 FeatureInstance membership、persistent `SceneData` provider/capabilities、View set、scene-local GPU semantic resource state/ids，**不拥有** Runtime、VulkanDevice global owner、Scene ECS、Transport queues 或独立的全局 Graph compiler。`SceneCapabilityIndex` 是非拥有 cold compose index，真实 Provider object 在 FeatureInstance/SceneData authority 下存活。

同一 Scene 可以同时有 Mesh、PointCloud、Terrain、Canvas2D、Gizmo 和 UI Pass，所有消费者使用同一有序 FrameExecutor/Graph execution，而不是各领域拥有第二套 RenderFrame。数据 Provider 一旦安装，其 SceneData 持久存储；View 只有 Camera 变化时无需重传 Mesh、重新创建 GPU buffer 或重建 Graph。

## 8.4 Frame scheduling（Simulation/Publication/Rendering 独立）

```text
Simulation 10 Hz:   S0 ---------------- S1 ---------------- S2
Publication:        P0 ---------------- P1 ---------------  P2
Render 60/144 Hz:   F0 F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 ...
```

FrameExecutor 使用最近一次**完整提交**的 RenderScene state。合法 Target/ready/pacing 下，即使没有 PROGRAM 更新仍可渲染；Transport 被背压不自动暂停 Present。Renderer FrameSerial/RenderTime、Simulation revision、Publication revision、GPU completed serial 是四个独立数值，不允许假定相等。

`RenderRuntime` 的等待/唤醒必须同时考虑：Transport epoch、新的 Control/Upload/diagnostic、Target acquire/present readiness、明确离屏需求/测试帧触发、帧 deadline/vsync、GPU in-flight capacity、Stop/terminal failure、resize/wake。不能 `while(true) if(queue.empty) sleep_for(1ms)`，不能 `atomic.wait` 在没有 packet 时永久阻塞帧 deadline，也不能在不可渲染 surface 上 CPU 忙轮询。

**Camera 独立性**：Editor UI Camera 可经 PROGRAM 的 per-view data 或合法 backend-local controller 更新独立于 Simulation，仍需通过受控线程域和版本保护提交；Backend 不得每帧读取 ECS live mutable state。没有通用 TimeManager/InterpolationManager；若具体 Feature 需要 camera/transform interpolation，约束其 History/Source Time 在该 Feature/View 范围内。

## 8.5 统一 FrameExecutor 的多 Scene、多 View、多 Target 算法

1. Backend frame scheduler 获取当前所有 active target 与对应可渲染 `(Scene, View)`，检查 SceneData/Graph readiness/generation。
2. 按 Scene 与 RenderPath（native compile key）形成**临时**可兼容 View groups；同 RenderPath 的多个 Camera 不重复编译 Graph。
3. 按 `ExecutionScope::Scene` 的**真实输入相等和本次 invocation key**去重共享计算（例如 Scene GPU Skinning）。不能因同名 Pass 就合并不同姿态/时间/阴影投影。
4. View-local pass 对不同 Camera/extent/history 保持自己的输出；如果 Vulkan Multiview 设备与 Shader/visibility/target compatible 可合并 Draw，否则普通分别 Record。同 View 的多个 Target 如果真正结果可用则可复用对应 buffer/image，但必须满足格式、内容、读写/目标 lifetime 合同。
5. Target-level pass 处理 ordered composition、UI overlay、present/readback/export；所有 Acquire/Present/semaphore/fence 统一交给 VulkanFrameDriver。
6. 记录 Scene/View/Target work counters，编译计划 cache_hit/record_cpu/frame_submit、GPU timestamp和实际 in-flight 资源 use；无新 PROGRAM 也可执行新帧绑定。

`ViewGroup` 为预留 scratch value 而非长寿 owner；**唯一 FrameLoop** 在 RenderRuntime/VulkanBackend，Feature 不得拥有自己的 tick/submit loop。

## 8.6 Target 类型与合成

`RenderTarget` 可以是 Surface Swapchain、Offscreen Image Pool、Readback/Image Export。一个 Target 可以按明确 Layer 顺序组合多个 `(Scene,View)` 的输出与 UI 叠加；同一个 `(Scene,View)` 可以绑定多个 compatible target（必须区分重用结果与重新渲染）。Target 核心状态只包括必要 `extent/format/sample/generation/acquire/resize/rebuild/present`，不重新实现 separate UI-specific NativeBackends。

Resize/present-out-of-date：先 prepare new candidate、验证 format/layout/owners、在 safe transition 发布，旧 swapchain/attachment 直到最后 GPU/Present usage 完成才退休。不得在已 reset fence 的帧中间强行 swapchain recreate 导致死锁；Surface minimize 无 active target 是合法 idle，而不是 RenderRuntime fatal 或无限循环。多 Surface 同一帧可能有不同 acquire/present availability，提交记录必须对应真正 acquire 成功的 Target。

Readback 由 Target/Feature拥有 staging/readback native backing，CPU 结果通过 bounded request/receipt 返回，可与 Scene/View/Frame/Request generation 绑定避免过期 Picking/截图被误投送。

## 8.7 Scene/View/Feature 生命周期顺序

`CreateScene` 由 CONTROL request admission，Backend 完整创建 candidate Scene/providers/Feature members、返 SceneId/receipt，再允许发布 Scene state；`CreateView/Resize/Remove` 经同类 generation+receipt，不允许 Source 端提前猜 GPU View 已经存在。View remove 必须先撤消 Camera/History/Feature-local state 的未来引用，GPU-safe retire 等待最后 submission；Scene remove 先卸载/失效 dependent Views/Features/capabilities，再回收 Scene resources。

连续 create/remove/recreate 相同 numeric handle 时必须检查 generation。Backend 未消费旧 PROGRAM 时，Scene/Feature/Resource 创建/删除次序由 acceptance/receipt 因果合同负责，不能依赖跨 Lane 偶然排序。RenderSystem 发布的 retained candidate 不可被新一轮 Scene remove 清理不安全 bytes，需明确 terminal cancel/retire policy。

## 8.8 错误状态、Device Lost 与关闭

正常 Device/session：`ACTIVE → STOPPING → STOPPED`；`VK_ERROR_DEVICE_LOST`：一次 terminal failure，所有新请求返回 `kDeviceLost`/`kTransportStopping` 之类结构化错误，已接纳的 pending replies 可读或 abandon，诊断持久记录首因，FrameExecutor 不再录制/提交。没有隐式自恢复/第二 device，除非将来独立正式设计并验收。

析构必须在约定线程/domain，完成停止/wake、CPU callbacks join、GPU inflight retirement（正常）或设备损失时 safe teardown；不在普通场景资源销毁使用 `vkDeviceWaitIdle`。`RenderRuntime` 的 owner 销毁前 Scene-side ResourceDomain/RenderSystems/Plugins 按真实 pin/lease 完成关闭，不随意 `shared_ptr` 引入循环。

## 8.9 真实最小到完整的阶段验收

早期真实 GPU vertical slice：one Runtime → Backend thread → one Scene → one View → one offscreen clear color Graph → readback；随后真实 Surface/Present，24+ sequential frames 无 PROGRAM 仍正确，pacing 不忙等。之后 1 Scene/4 Views、multi-scene/two-scene-shared-asset、same path shared native plan、different paths independent, per-view history, Multiview GPU readback fallback、multi target composition、resize/minimize/device-lost/stop/recreate/error/SDK consumer。完整场景 parity 与性能在第 10/16 章。
