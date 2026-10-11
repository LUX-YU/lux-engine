# RD2-F5｜真正 Runtime 的入场合同（未来备用，当前禁止直接执行）

> 这不是当前用户授权。必须先 RD2-D0 用户批准、F4-C 报告以 F4 PASS 通过并再次明确授权 F5 后才能实施。

## 目标

实现一个 `RenderRuntime` + 一个 `VulkanBackend` + 唯一 `FrameExecutor/Driver`，真实 RenderScene N、RenderView N、Target N、Graph logical/native plan cache、GPU slot admission/completion、Present/Offscreen/Readback、Target resize/minimize/lost/stop、独立帧进度与合理帧 pacing。为未来 RD2 Scope-owned DataSets/NativeStorage 保留明确的**归属、borrow、owner identity、被动 retirement 接缝**，但不抢跑 F6 的 DataSchema/外部插件或 F7 的真正稀疏 GPU Storage。

## 首选类/算法外观

`RenderRuntime` 是真实 authority，`VulkanBackend` 负责 native resource/plan owner，`RenderScene` 管 Scene Scope 与 Feature membership（初期固定 test consumer），`RenderView` 管 Camera/History/Target，`FrameExecutor` 表达唯一执行算法；除这组确有职责的 owner 外，默认免费函数、值类型、短期 scratch。Feature Hook 先使用 F5 授权固定测试路径，不引入未实现的“类型注册成功”的假 API。不得创建 `FrameScheduler`, `GraphicsManager`, `GPUDataManager`, `ViewGraphManager` 多层转发。

## 与 F4 的关键接口演进

`ExecutableGraphPlan` immutable shader/native recipes 跨相容 View 共享；动态 `InvocationSlot` 必须以正确的 Frame Submission/queue tickets 回收，不能把 frame_capacity 误当 view_count。至少 4 View 可以同一帧合法独立执行，且共享相同 compiled plan；每 View 的 Camera/History/Target backing 隔离，Scene-scope Skinning test 只执行一次（若严格输入相同）。本阶段可用固定的合成 Graph/Data test fixture 证明接口，但生产 DataSchema/TypedStorage 在 F6/F7 交付；F5 不借“尚未有数据”缩减 RenderRuntime 的要求。

## 权限（实际开工再枚举具体文件）

**ALLOWED**：`modules/function/render/runtime/**` 或经已批准 FINAL 所定义的 `render_runtime` 真实 target，`modules/function/render/vulkan/backend/**`、Frame/Scene/View/Target、bootstrap tests/真实 Presentation integration；少量 F4 Native Plan 公有接缝必须单独影响审查。

**READ-ONLY**：F1–F3 Schema/Graph/Shader/Transport/Description、old F4 I/V reports、RD2 文档与原 FINAL；**FORBIDDEN**：Legacy 719 blobs、Editor/Scene ECS/Feature concrete/外部 SDK 全面迁移、第二 Renderer 或新的通信 Lane。进入阶段前更新真实仓库路径/依赖/ALLOWED 清单。

## 验收

1. one Scene/View real GPU offscreen/readback/Present；多 Target Layer 正确；
2. 1 Scene/4 View 相容 Plan 编译次数=1（结构性 BindingSlotKey 不含某 Scene 的 DataSetId），Shader/native owner 无复制，四份 View outputs 不同且正确，合法独立 slot；
3. SceneScope GPU work 一份，View work 四份，切 RenderPath/Target 或删除 View 不破坏其他 View；
4. 2 Scenes shared asset Native backing 不重复上传，但 Scene Visibility/History 分离；
5. 10Hz Sim / 60–144Hz render，无 PROGRAM 不忙等且仍能更新 View/UI；
6. 真实 resize/minimize/Device Lost/Stop、GPU inflight retire 和失败回退，GPU Validation/SyncValidation 0 Error；
7. 固定 warm frame 0 first-party heap/lazy native create，稳定 p50/p95 与同质量 baseline；
8. I 与独立 clean-clone V，保留全部 F0–F4 tests 和 frozen 719 blobs，PASS/PARTIAL 均 STOP，用户审查后才 F6。

## F6/F7 预先禁止事项

F5 不创建 `DataRegistryService<T>`、万能 ECS Store、虚 `IFeature`、第二 Shader/PassSchema 工具链；只为后续 Data-First Scope owner 保持极少的值型 slot/binding 接缝。`Feature` 与 `DataSet` 不能是同一对象，也不能要求所有 View/2D 产生 MeshInstance。
