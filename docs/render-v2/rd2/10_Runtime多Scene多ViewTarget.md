# 10 — 单一 Runtime、Scene/View/Target、存储作用域与帧执行

## 10.1 一个真正的执行域

终态恰好一个 `RenderRuntime`、一个 `VulkanBackend`、一个 `FrameExecutor/FrameDriver`。Engine 只有一个 Scene RenderSystem 作为发布源（每 Scene），Editor、Player、Tools 皆使用同一个 typed transport/render entry。Feature 没有自己的 FrameLoop，Canvas/Gizmo/UI 不创建独立 UI Renderer。Runtime 无 PROGRAM 变化时仍可按 Frame Pacing（10Hz simulation → 60–144Hz render）提交合法帧；FrameSerial 和 GPU completion/diagnostics 独立计量。

## 10.2 Scope 与数据使用

- **Runtime Scope**：跨多个 Scene 真正共享的资源集合（如 cooked Mesh/Texture/Material Asset 的 GPU backing cache）；但逻辑资产 retain 仍由 CPU `RenderResourceDomain` 决定。
- **Scene Scope**：Light/Transform/MeshDraw/Sprite/PointCloud/Scene settings 等不同 Scene 不能默认混同；同 Scene 多 View 可共享稳定 DataSet/Storage。
- **View Scope**：Camera/jitter/HZB/TAA/temporal feedback/per-view visible lists 等；每 View 的历史 epoch、resize/camera cut/rebase invalidation 独立。
- **Target Scope**：Swapchain/Offscreen/rendered layer/shader outputs，真实 present/format/sample/extent/resize 生命周期。
- **Graph Transient**：不等价上述永久 Scope；只管理有明确 GPU lifetime 的中间结果。

Type 具有默认/允许 Scope，DataSet 创建时决定某份实例实际所属 scope。不能把 `scope=scene` 写死为某个 C++ 单例，也不能让一个跨两 Scene 共享的 Mesh 资产错误共享 Scene visibility/cull。

## 10.3 FrameDriver 的唯一调度逻辑

1. 拿到最新已 admission 的 CONTROL、PROGRAM 与 UPLOAD，按因果 receipt 更新 Scope data；不在帧循环里扫描全部 ECS。
2. 根据 Target deadline、有效 Scene/View/RenderPath，选用已编译 plan；只有 schema/topology/native key 改变才编译候选。
3. 验证 DataSet GPU-ready snapshot 和写入冲突；制作一组有界 `FrameGraphBindings` / `NativeImportBinding`，pin NativeResourceOwner。
4. 对 Scene-scope pass 构造共享工作 key（data revisions, scope, time, side effects），相同并可证才执行一次。
5. 对每 View 构造 camera/history/target-specific invocations；可选合法 Vulkan Multiview 合并，不合法走 separate view fallback。
6. 按 Native Graph Queue admission 录制/提交；所有 Export/Present/Readback 有真实 layout, external use, completion。
7. 轮询/回收已完成的 slots/native backing/retired plugins/query，不阻塞正常帧；Device Lost 令当前 Runtime 终态失败；宿主只能按后续授权另建 Runtime，不在此引入自动恢复状态机。

`FrameClock` 的模拟/渲染分频不意味着每个 Simulation Tick 对应一帧 Render。无新业务包时仍可绘制 Camera/Editor UI; dirty-free Scene permanent buffers 不需要重新上传。

## 10.4 N View/Target 可伸缩性门槛

F5 必须实测：一 Scene 四个 View 使用相容的 Plan，**只编译一份** Shader/Native immutable plan，每 View 有独立绑定/历史/目标，四个可合法 in-flight；相同输入的 Scene Pass 确实只执行一次，View Pass 四次；一 View 切换 RenderPath 不影响其它 View 的 DataSet；不同 Target 格式/尺寸若不相容必须产生可证明的 Native key 分叉。Frame-capacity/Invocation-slot-capacity/number-of-views 三者分开，不按数组索引取巧。

## 10.5 Scene/File/Feature unload 与资源留驻

删除上游 ECS Entity 只取消对应 DataSet record/generation；DataSet 本身是否存在取决于所属 Scene/Key/retention，GPU physical allocation 可在最后 GPU 使用后保留容量以供复用。Feature detach 不自动删除 shared DataSet。删除一个 Scene 应禁止它的 Consumer、Retire 其 Scope data/backings，但不能影响另一个 Scene 共享的 Asset GPU backing。Target resize/minimize、Swapchain image acquired/present、历史 epoch、设备丢失要有真实原生证明；不能测试无窗口小例后宣称产品已可用。

## 10.6 Layout 共享与 Native Backing 的实例化

RenderPath 编译只含 `BindingSlotKey`、DataSchema ABI、GPU Device/Shader/PSO、Target compatible shape；不同 Scene/View 的实际 `DataSetId`、Record revision 与 native image/buffer handle 在同样结构时不产生新的 Pipeline Layout。GPU backing 每次由对应 FrameInvocation 的 `DataSetId→NativeResource` 绑定并提供 Ready/Last-use。若 StoragePlan 改变物理 shader layout、descriptor shape 或设备 capability 不相容，新的 Native Plan 才需 cold compile；只增长容量且相容的 buffer 可以只安全更新 Descriptor/Backing snapshot（需退休旧）。
