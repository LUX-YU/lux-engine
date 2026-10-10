# R0：V1 producer 与跨 Lane 迁移输入

来源固定为 `a669409a289a6fa4092f21176397795b1cdb7f3e`；下述原路径已映射到
`render_legacy/<original-path>`，未迁移的 Editor/Scene consumer 仍位于原处。
本文件只记录代码审计与未来测试向量，不将未运行的 V2 行为标记为 PASS。

## 实际入口、线程与拥有关系

| 路径/入口 | 线程与证据 | publication / completion / lifetime |
|---|---|---|
| `engine/scene/builtin_systems/render/src/RenderSystem.cpp`：`submitProgram` / `publish` | SceneDriver 执行场景阶段；调用 `RenderRuntime::submit`，入口 `Impl::check` 检查创建线程 | Stage 准备完整 `pending_`；commitPrepared 后 retained owning packet 重试；成功后 commitViewPublications |
| 同目录 `Builtin3DRenderStages.cpp`、`CameraExtraction.cpp` | Stage 接收上项 builder，不独立发起线程/队列 | Mesh/Light/Camera 状态写入同一 PROGRAM；观察存量、dirty 与 full-sync 是迁移合同 |
| `editor/app/src/UiRenderSyncStage.cpp` | UI stage 在同一 Scene publication 调用链内，非独立 Renderer | appendFrame/appendClear 使用同一 builder；shared UI frame 被 packet 保活；连接观察者时折入已存在 UiFrame |
| `modules/function/ui/rendering/src/RenderClient.cpp` | 只向 caller 的 PROGRAM builder 写入 | UI attachment 随 owning packet；不增加独立上传/调度 authority |
| `RenderResourceScenes.cpp`、`RenderResourceViews.cpp` | RenderResources owner 侧装配/poll；调用 runtime.control()，由 Runtime 检查线程 | createScene/addView/createTarget/switch/resize/remove/destroy 的回执分别维护；不可假设仅提交即 ready |
| `RenderResources.cpp`：request/find/adoptCompleted | `requireOwner()` 对线程不匹配执行 renderFatal；工作通知通过 CompletionWork | 资源读取异步完成后在 owner adopt；记录 retain/submission，不能用最后 CPU 引用直接决定 GPU 销毁 |
| `RenderResourceAssets.cpp` | 通过上述 owner adoption 路径取得 Runtime upload client；shader compile 使用 CONTROL | Mesh/Texture/Material 上传产生 request，ready 后才提供 resolved handle；retire 检查未完成 submission |
| `engine/context/rendering/src/RenderContext.cpp` | CompletionWork 装配到 ExecutionRuntime owner；wake callback 只请求工作 | collectCompletions → submitPending；Feature registration completion/rollback；Runtime 与资源任务域由同一上层装配 |
| `modules/function/render/runtime/src/RenderRuntime.cpp` | Impl 保存构造线程 ID；control()/upload()/submit()/progress 检查 owner；backend 为独立 jthread | PROGRAM/CONTROL 使用专用端点；maintenance PROGRAM 也由 owner submitPending 产生 |
| `modules/function/render/runtime/pinclude/lux/engine/render/detail/UploadQueue.hpp` | 复制出的 UploadClient 可被多 producer 使用；队列通过 mutex 保护容量/字节账本，owner poll 再送 UploadSession | V1 是 MPMC admission + owner-side SPSC transport；不能把“MPMC handle”误写成 V1 的每级物理队列都为 MPMC |
| `modules/function/render/client/include/lux/engine/function/render/client/RenderUploadClient.hpp` | 获取 client 经 Runtime owner；持有 client 后其 submit 可跨 producer | owning builder/blob/attachment，显式容量结果；返回值保有 reply state，禁止借 caller 临时内存 |
| `examples/render-plugin/ExternalFeatureTest.cpp`、installed render/spatial consumer | 独立 host 的主执行流程创建 Runtime、注册和等待 | 用于后续 SDK 对照；不推导任意第三方插件都遵守 owner-thread 合同 |

检索候选全集及准确行号位于 `V1_SOURCE_AUDIT.json`。候选包含声明、handler、测试和 ScriptRuntimeSystem
中非 Render 的同名 submit；词法命中不等于实际 Render producer。上表按真实调用链分类，
不把 280 条命中宣称为 280 个已动态验证的生产者。没有运行线程追踪，也没有外部插件全集证明。
后续引入新的 producer 必须审阅其实际执行域；出现独立线程直接发 CONTROL/PROGRAM 时汇合到 owner，
不得静默放宽 V2 SPSC 或直接把未知并发调用塞入单生产者队列。

## 必须进入后续阶段的测试向量

| 向量 | 观察点与拒绝条件 | 阶段 |
|---|---|---|
| createScene/addView 与 PROGRAM 交错 | accepted 与 ready 分开；未 ready 不执行依赖操作 | R2/R5/R7 |
| UPLOAD 延迟而 PROGRAM 先到 | resolved handle 仅在 receipt 完成后发布；拒绝未 ready/stale generation | R7/R9 |
| destroy 与 queued packet / in-flight GPU 并存 | 停止新引用；保留旧 packet backing，fence/retire serial 证明后销毁 | R4/R5/R9 |
| PROGRAM 队列满 | 保留并重试同一 owning packet，不覆盖、不复制提交、不自旋、不无界 fallback | R2/R8 |
| retained 期间 create→update→update→remove | observed revision/dirty/departure 继续累积；旧 candidate 接纳后发布后续变化 | R8 |
| 连接发生在组件创建之后 | 折入存量；直接字段改写不触发 EnTT update，观察写入使用 patch/replace | R8 |
| 资源稍后 ready | 异步 completion/adoption 触发后续 membership；不能仅依赖一次 on_construct | R8/R9 |
| 两 Scene 使用同一资产 | 一个 runtime integration ResourceDomain 负责 Scene-side authority；不重复拥有 GPU backing | R9 |
| stop/device-lost 与 pending reply/upload 并发 | 结清 owning state/字节预算并停止新工作；Device Lost 不恢复同一个 Runtime | R2/R5/R9 |
| Feature code/provider 卸载 | code pin 覆盖最后回调和析构；consumer 先于 provider 拆除 | R6/R12 |

这些是源审计交接，不是 V2 GPU 运行证据。R0 不修改上述算法，也不建立新 transport、manager 或 revision framework。
