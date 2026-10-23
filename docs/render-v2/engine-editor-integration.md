# Engine / Editor 与 Render 的统一执行及接线设计

本文替代本文件的旧接线方案。源码核对基线为
`1e3249e7eb51a8ef1d94a304f9edb9078d8bcb1e`（2026-10-23）；下文明确区分现状和拟实施方案。
本文说明接线目标与实施边界；API 已迁移不代表窗口输出、产品闭包或性能已经验收。

遵守 [工程原则](../engineering-principles.md)、[Context 扩展合同](../context-extensions.md)、
[Editor Framework v2](../editor-framework-v2.md) 和 [Editor 质量规则](../editor-quality.md)。
Render 当前链路见 [源码阅读说明](rendering-flow.md)。

## 1. 目标与设计裁决

终态是一个应用执行体系：Engine、Editor、资产解码、世界加载和 Render 的 CPU 工作共用一个
`ExecutionRuntime`；阻塞 IO 使用它配置的独立阻塞执行资源。Main、Asset 通信线程和 RenderThread
分别推进各自有真实线程归属的工作，不创建额外计算线程池。

**现有类型和 API 不是必须保留的边界。** 可以修改、合并和删除不合适的设计；保留的是业务语义、
数据寿命和有效算法。不要为复用一个旧接口增加转发层、重复终态、额外队列或无意义的线程切换。
新类型必须说明自己拥有的实际对象以及替代的旧实现，不能以 Manager、Host、Owner 等笼统名字掩盖职责。

固定原则：

- stdexec 负责通用异步 OperationState；业务对象只保存业务状态、结果和必要身份。
- 完成一个计算、交付读取结果、采用业务状态是不同动作；只有采用需要进入对应业务安全点。
- 调度策略写在实际切换处。不能因为来自 Editor 就在阻塞池做解码，也不能让 CPU worker 阻塞等 IPC/磁盘。
- Command Response 保留显式错误字段；内部函数按语义使用 expected/bool，不层层再包一套结果。
- 保留 stdexec 上传、真实 FIF、单 RenderThread、唯一物理清理 `DeviceSession::cleanup_scope_`。
- Task/项目取消域不是物理清理域。不增加 Arena/Storage 局部清理 Scope，也不把 TaskId 当 GPU 完成身份。
- 外部输入、代际、真实容量、设备失败在相应边界检查一次；稳定内部路径不反复查类型、名称、线程和相同范围。
- 调度、运输与数据采用分开，但不为每一步创建对象、分配状态或增加一跳。
- 无论采用何种标准组件，都必须给出实际删除收益、调用链和分配成本；不用“用了 Sender”代替简化证明。

本轮不重写 MaterialGraph、阴影算法或 RenderGraph，不恢复 Legacy RenderRuntime/Readback，不新增通用服务容器。
窗口、UI 和 Scene 接线是实际产品范围；阴影质量和专项性能优化后置，未归因性能保持 PARTIAL。

## 2. 当前事实与未完成部分

| 事实 | 源码依据 | 本轮处置 |
|---|---|---|
| EngineContext 已唯一创建 ExecutionRuntime，SceneRuntime 借用其 CPU Scheduler | [EngineContext.cpp](../../engine/context/src/EngineContext.cpp) | 保留，不再把共享池写成待从零实现 |
| CPU 后端已为 stdexec static_thread_pool；阻塞 IO 有独立 bounded workers | [ExecutionRuntime.cpp](../../modules/core/execution/src/ExecutionRuntime.cpp) | 一处配置；区分计算和阻塞，不把“同一个 Runtime”说成“所有工作同一批线程” |
| TaskExecutor 已无私有池，整图 Sender 使用 CSR 后继算法 | [TaskExecutor.hpp](../../modules/core/task/include/lux/engine/task/TaskExecutor.hpp) | 保留 O(V+E)、affinity 和 workspace；不以 worker_count=0 规避接线 |
| RenderServer::create 已接收 ExecutionRuntime&，不创建 CPU 池 | [RenderServer.cpp](../../modules/function/render/src/runtime/RenderServer.cpp) | 实际应用组合根尚需接入 |
| Render 已有标准 Scope、完成调度、完整 SubmitFrame 和真正 FIF | [FrameSubmission.cpp](../../modules/function/render/src/runtime/FrameSubmission.cpp) | 复用，不能再造 PendingOperation 或外层帧循环 |
| RenderRequester 已有 typed RequestSender/FrameSender、关联表和 L+1 输入载体 | [RenderRequester.hpp](../../engine/rendering/include/lux/engine/rendering/RenderRequester.hpp) | 扩展实际装配，不能再次实现同名桥接 |
| TaskScope 的 connected operation 已交给 stdexec；结果交付由 Sender 显式表达 | [Task.hpp](../../modules/core/execution/include/lux/engine/core/execution/Task.hpp)、[Task.cpp](../../modules/core/execution/src/Task.cpp) | 任务监控、逐任务取消和实际代码寿命仍保留 |
| TaskScope 只有一个工厂式 submit，已删除隐式 Completion 参数 | [TaskScope.hpp](../../modules/core/execution/include/lux/engine/core/execution/TaskScope.hpp) | 业务结果在流水线消费，Runtime 只记录终态 |
| VFS read 在有限 IO Sender 内直接完成端口交付 | [VfsAssetReadEndpoint.cpp](../../engine/process/asset_loading/src/VfsAssetReadEndpoint.cpp) | Main 只承担确有需要的业务采用 |
| AssetLoadSender 已用标准 Sender 表达读取→CPU 解码 | [AssetLoadSender.hpp](../../engine/process/asset_loading/include/lux/engine/process/asset_loading/AssetLoadSender.hpp) | 已删除专用转发 Receiver/TState，停止与错误仍准确传播 |
| 世界解码的有限 coroutine 保持 CPU affinity | [WorldPartitionLoadSender.cpp](../../engine/process/world_loading/src/WorldPartitionLoadSender.cpp) | 每次读取后执行原 decoder.accept；Scene 采用单独进行 |
| Editor 项目准备已用同一 Runtime + ObjectScheduler | [LuxEngine.cpp](../../editor/app/src/LuxEngine.cpp) | 保留项目序号和安全采用；删除完成回传的空绕行 |
| WorldFileStorageSource 提供正式有限文件范围读取 | [WorldFileStorageSource.cpp](../../engine/process/world_loading/src/WorldFileStorageSource.cpp) | 测试只保留延迟控制；实际 IO 复用该 provider |
| EngineContext、Scene 和 EditorUiScene 已迁统一运输；旧 UiFrame/UiRenderSyncStage 删除 | 各模块 CMake 与 Editor app | 根 EDITOR 使用真实 Engine/Render 依赖，不加空 target/兼容别名 |
| SurfaceTarget 已使用 NRI SwapChain；Target 图像输入与 Resize 有实际消费者关系 | [SurfaceTarget.cpp](../../modules/function/render/src/execution/SurfaceTarget.cpp)、RenderServerImpl | 窗口与 Scene 输出接入现有 FIF，不创建呈现调度层 |
| CPU UI 捕获中立 UiDrawData，Editor 直接写入共同 FrameInput | [DrawData.hpp](../../modules/function/ui/include/lux/engine/ui/DrawData.hpp)、[EditorUiScene.cpp](../../editor/app/src/EditorUiScene.cpp) | 顶点与索引只捕获一次；Native 只补齐复制尾部 1–3 字节 |
| Raster 已具有 IndexedDrawList、逐 draw 裁剪、图像选择与颜色混合 | [Draw.hpp](../../modules/function/render/include/lux/engine/render/graph/Draw.hpp)、[RenderExecutor.cpp](../../modules/function/render/src/execution/RenderExecutor.cpp) | UiRasterFeature 只消费中立数据，不链接 ImGui/Editor |

旧文档中“Asset Client 回主线程发送”“TaskExecutor 尚有私有池”“新建 RenderRequester”均被本版取代。
历史 Root EDITOR 曾缺失 `engine_context_render`、`project_plugin_rendering`；活动配置已迁入真实
`engine_rendering`/`scene_render`，旧包不再作为依赖。历史失败不回写，产品验收绑定最终精确提交。

## 3. 线程、执行资源与唯一权威

### 3.1 执行位置

| 执行位置 | 执行内容 | 不承担 |
|---|---|---|
| Main / Object 线程 | 窗口输入、LuxObject、Scene 阶段、业务采用、Main Client、冻结完整帧 | 文件读取、大块解码、GPU 录制 |
| ExecutionRuntime CPU 池 | TaskGraph worker 节点、资产/世界解码、Render 编译及上传 CPU 准备 | 永久 IPC pump、同步等待同池后继、阻塞文件 IO |
| 同一 Runtime 的 BlockingScheduler | 有限文件/设备阻塞调用、确实阻塞的插件准备 | 解码、持续 Render/Asset 事件循环 |
| Asset 通信线程 | 一个 Asset Requester 的发送/收取与有限 continuation | 第二份资产业务目录、文件读取、解码、GPU API |
| RenderThread | Frame/Command 接纳、设备资源发布、录制/提交、GPU 完成、Outbox | Engine Registry、UI 对象、磁盘 IO |
| ObjectRuntime 队列 | 目标代际、回调借用和业务对象线程交付 | 通用计算、IPC 关联和 GPU 资源清理 |

Asset 通信线程是一个可唤醒的串行协议执行线程，由现有应用组合根中的 RenderExecution 管理。
它不是 CPU 池，也不以 sleep 轮询。不能把永久 pump 放进共享 CPU 池，永久占住一个计算 worker。
相同 CpuScheduler 的不同任务可落在不同线程，因此它不能代替 SPSC Link 的固定线程归属。

Core RunLoop 是现有线程驱动的标准 Scheduler，不拥有线程。Main、Asset、Render 各自需要自己的就绪归属，
可以使用同一个实现；不能把不同线程的安全点合成一个进程队列。ObjectScheduler 还提供对象代际保护，
保留这一独立语义，不变成普通 RunLoop 的别名。

### 3.2 装配与对象寿命

`EngineContext` 继续拥有 ExecutionRuntime、AssetVfs 和 SceneRuntime。
删除它当前失效的 `RenderContext/rendering_/EngineRenderAccess` 接缝，不再给 headless Context 增加可变渲染设施。

实际应用组合根显式拥有 `RenderExecution`、`RenderAssets` 和窗口：

1. 创建 ObjectRuntime 和 EngineContext。
2. 用 `engine.execution()` 创建 RenderExecution，再建立 Main/Asset 两个 Requester。
3. 构造 RenderAssets 和 Scene 所需的窄依赖。
4. 构造 EditorContext、窗口对应的 EditorUiScene 和项目 Scene。
5. 关闭按相反依赖关系清偿，不能只依赖成员析构自动等待业务。

Scene 使用已有 SceneCapabilityProvider/构造参数取得依赖；Editor 的 ContextExtensions 只在冷态绑定已经存在的对象。
不在 EngineContext 发布后的扩展表补写 RenderExecution，也不通过查询偷偷创建服务。

| 类型 | 实际责任 |
|---|---|
| `RenderExecution` | 一个 RenderServer、RenderThread、Asset 协议线程及其 RunLoop；组合两个现有 Requester，管理启动和关闭 |
| `RenderRequester` | 一个 Link 的 Command 绑定、有限关联与输入载体；不创建线程，不拥有 connected operation |
| `RenderAssets` | Main 线程的 AssetId/版本到真实 Render 句柄、业务使用关系与采用事实 |
| `RenderSystem` | 从 Scene 提取增量输入，维护该 Scene 的非拥有渲染关联，向共同 FrameInput 追加命令 |
| `EditorUiScene` | 独立 Render Scene/View/Effect 和 UI 帧捕获；不再创建空 Engine World/Simulation |
| `UiRasterFeature` | 将中立 UI 绘制数据降低为既有 Graph/Raster 工作；不认识 ImGui Context、Pane 或项目 |

这些对象分别承担实际应用设施、业务资源或 Scene 职责，不添加统一 Manager/TaskManager/UploadManager。
RenderAssets 不另管 GPU 物理对象；物理数据仍由 Render Storage 和唯一 cleanup_scope_ 负责。

## 4. Engine / Editor 异步收敛：目标形状

### 4.1 消除隐式完成回传

最终每条有限业务流水线直接表达：

```text
接纳与捕获输入
  → schedule(blocking)：确有阻塞的读取
  → continues_on(cpu)：实际解码/计算
  → 必要时 continues_on(asset)：发送和收取 GPU 上传 Response
  → 必要时 continues_on(main/Object)：采用业务结果
  → 最终任务状态
```

没有 IO 或没有采用动作就省去对应切换。运输结果不能先绕回 Main 才开始解码。
每条流水线先处理真实错误和已发生的副作用，再决定是否还能向已经关闭的 UI/Scene 交付。

TaskScope 收敛为“有限任务准入、取消分组、监控”，使用一个工厂式 submit；
删除 `submit(options, factory, completed)` 的隐式结果采用模式。
`ExecutionRuntime::submit` 同样迁移，保留需要 Task 句柄的实际消费者。

最终 Sender 在自己的组合中消费业务值和错误；Runtime 只记录成功/失败/取消及必要诊断。
不建立新的通用 ResultManager，也不让每个完成结果在 Runtime 与业务对象之间反复搬运。
任务入口接收有限 Sender：业务数据已经被实际消费者采用或保存，末端仅保留 void/明确的任务失败通道。
不能在业务失败后用无条件 set_value 把监控状态写成成功。已有 expected 错误、执行错误和 stopped 在任务终态
归一化一次；这不改变 Render Command 的显式 Response 合同，也不额外分配通用结果信封。

### 4.2 通用 OperationState 交给 stdexec

现有 TaskRuntime 内一个应用寿命的 `stdexec::simple_counting_scope` 接管 connected operation。
选择 simple 版本的原因是任务已有真实的逐 Task 取消源和项目取消组；Runtime 停止通过现有取消入口传播，
无需再引入 Scope 的第二套停止源。它属于现有 ExecutionRuntime 实现，不是新类型层，
也不代替 DeviceSession 的物理清理 Scope。Render 已有 counting_scope 不因此机械改成同一种类型。

- 删除 `TaskOperation` 虚接口、`TTaskOperation`、`startTask(record, unique_ptr<...>)` 及
  `TaskRecord::operation`。TaskScope 析构只停止所属业务组，不销毁或阻塞等待仍被接纳的 operation。
- 保留 TaskRecord 的 TaskId、进度、历史、父任务、真实逐任务取消和代码寿命。
  Editor 有实际单任务取消消费者，不能照搬 Render 删除所有逐请求 stop source 的结论。
- 业务结果直接交给流水线中的实际接收者。删除用于隐式业务 Completion 的 result_/deliver/
  deliveries/delivery_batch 路径；监控变化通知可以合并，终态及资源释放不能丢。
- 任务执行槽位的回收不依赖 UI 刷新任务面板。完成监控可以稍后派发，但不能因 Main 暂停而让已完成 IO
  永久占住接纳额度，妨碍同一已接纳流送工作的下一段。
- Scope 准入/close/join 和 stop 环境必须在本模块验证：保留实际任务 token，与 Runtime 停止意图衔接一次；
  不无条件套可取消的调度器吞掉已确定的副作用结果。
- 代码 pin、TaskReporter 的有效期和 Producer 回调尾栈必须覆盖真实访问。
  Scope join 不是“所有外部 Producer 已退栈”的证明。
  RunLoop 从任务环境取得既有代码 pin，执行前移到 Core 调用栈上，回调返回后才释放；
  不能在 stdexec 销毁 operation 时同时卸载尚在返回的 DLL。此处只保活回调代码，不拥有 operation。
  任意外部 Producer 自己的回调尾栈仍须由其原生命周期覆盖。

这里不能预先承诺删除所有 starting/collected 类字段。实施先以真实消费者验证同步完成、start 尾栈、
取消回调、operation 析构和记录回收时序；只保留由这些不同寿命直接要求的最小保护。
不得把旧 Operation 指针/删除器换名放进 TaskRecord 或新 Result 对象。
若标准 Scope 方案需要再造同等复杂的执行状态 Owner，本模块不准宣称收敛完成，应重新缩小方案并报告依据。

### 4.3 统一 Main 的通用完成接缝

ExecutionRuntime 的通用完成交付复用现有 Core RunLoop 实现，提供表达实际线程的
`mainScheduler()` 与 `dispatchMainCompletions()`；仅用于绑定到应用 Main 的 Runtime。
它替代当前 Task 隐式结果队列和 CompletionWork 的独立 ready vector，不叠加第三条 Runtime 完成队列。

- stdexec continuation 直接排入同一 Main RunLoop；它保证交付已有结果，不因目标业务已关闭而擅自改结果。
- ObjectScheduler 仍在真正需要对象借用时使用；目标失效是“不能采用”，不等于运输未发生。
- CompletionWork 保留合并/撤销通知的实际语义，底层共享上述就绪通路。
  通知状态冷态构造，重复 request 不逐次分配。SceneRuntime 等实际消费者迁完再删除旧 vector/mutex 入队实现。
- Main 每次派发采用固定批次上限，不递归 pump；剩余工作留给下一安全点。
  Core ConcurrentQueue 不提供不同 Producer 的全局 FIFO，不据此推导业务顺序；有依赖的步骤在同一 Sender
  或明确的 Frame group 中串联。ObjectRuntime 原有固定批派发合同独立保持。
- 监控 API 与执行完成 API 命名分开。增加窄的 `settled()` 观测替代关闭时复制 `taskInfos()` 扫描；
  settled 的说明必须包括工作、交付和尾栈的区别，不能用一个 bool 冒充全部关闭证明。
- 等待所需的 mutex/condition_variable 不等于热路径必须持锁执行；采用现有 wake epoch 防丢唤醒，
  不为追求“所有地方无锁”改写真实任务目录与取消的冷路径。

需要调度回 Main 的业务在一处明确指定。已在 ObjectScheduler 的 value 回调中同步
`object::sendEvent` 不构成第二次跨线程跳转，不能仅按函数层数误删事件语义。

### 4.4 Editor、Scene、Script 的消费点

| 消费者 | 修改 |
|---|---|
| `LuxEngine::Impl::admitProject` | 文件和插件阻塞阶段用 blocking；确有重计算的阶段再切 cpu；最终通过已有 ObjectScheduler 交付 ProjectPrepared，不再绕 Task 的 completed 回调 |
| `receivePrepared/applyPendingHostChanges` | 保留请求序号、重入后的二次资格检查、候选采用和真实 published 事实；文件已发布后取消不能说成回滚 |
| `EditorContext::tasks_` | 保留项目取消分组，迁统一 submit；析构不 join 全局执行资源 |
| `WorldLoadingSystem` | 直接发布拥有型解码结果到现有 Read 状态；主阶段按 epoch/revision 采用，不让 Worker 改 Registry |
| `ScriptRuntimeSystem` 的 delay | Timer Sender 直接写实际终态/通知，脚本恢复仍在脚本 tick；删除只为 Task completed 存在的转发 |
| `ScriptAssetAccess` | 迁统一读取/解码/运输终态，保留 VM/访问票据与项目撤销；不扩大为 Script 架构重写 |
| `SceneRuntime` 的 Timer/CompletionWork | 保留实际定时与唤醒，接统一完成通路；只删重复转发，不删除定时状态或外部回调保护 |

TaskExecutor 的 DAG、PortSender 的外部端点适配、ObjectTarget 的代际以及 Script 的恢复记录都具有真实职责。
它们不因为包含 State/Callback/Queue 就自动属于删除项。

## 5. VFS 与世界流送

### 5.1 VFS 完成不能依赖 Main

当前链路：

```text
IO worker 读完 → TaskRuntime.collect（Main）→ Port complete → CPU 解码
```

目标链路：

```text
IO worker 读完 → 归一化读取结果 → Port complete → 共享 CPU 解码
                    └→ 任务监控另行通知 Main
```

修改 `VfsAssetReadEndpoint::Impl::submit`：读取、错误映射和 `Request::finish` 在同一个有限 Sender 中完成。
Endpoint 仍只负责准入、VFS 读取和端口交付，不负责 UI 结果采用。
不再经过 TaskScope 的第三参数 Completion。准入拒绝不调用端口完成，接纳后恰好一个终态。

保留而不重复包装：

- Endpoint 销毁只撤销新准入；已接纳请求持有所需 VFS 快照、输入和代码寿命。
- `AssetVfsView` 的 mount 快照是实际独立共享寿命，不套用 Render 物理资源“零共享”限制。
- 端口完成可能同步重入/销毁消费者；实际 read 结束即返还读取额度，然后交付结果。
  后续 continuation 可以立刻读取下一范围，不因上一段已经结束的 IO 被误拒绝。
  回调尾栈仍属于已接纳任务的真实寿命，额度归还不允许提前销毁执行状态、provider 或代码。
- 取消不能中断不支持取消的系统 read；其返回前输入与 provider 仍有效。
- 保留真实 max_bytes/请求额度；普通堆 OOM fatal，删除将其假映射为 BYTE_BUDGET_EXHAUSTED 的路径。
- `AssetReadOverlay` 的真实 provider fallback 保留；不为统一外观创建第二套 VFS。

`AssetLoadSender` 用 PortSender、错误映射及 CPU continuation 直接组合，
删除仅转发 set_value/error/stopped 的 `TAssetReadReceiver` 和等价手工 TState。
具体解码仍消费既有 TAssetSerDeser；不新增 Codec、资产目录或 DecodeManager。

### 5.2 世界解码必须在每次读取后正确调度

仅给 `loadWorldPartition` 外层加一次 starts_on(cpu) 不够：
异步 range read 完成可能在 IO 线程，当前 `complete()` 会在那里继续 `decoder.accept()`。
只在最终结果加 continues_on(cpu) 也太迟。

目标单分区链：

```text
CPU：decoder.start / 得到下一范围
  → ReadWorldStorageRange：内存可立即完成；文件在 blocking 完成
  → continues_on(cpu)
  → decoder.accept：校验/解压/组装，产生下一范围或完成
  → 发布一个完整 WorldPartitionData
  → Scene MAINTENANCE 验证 source epoch/demand revision 并采用
```

`WorldPartitionLoadSender` 显式借用 CpuScheduler；保留逐段解码算法，调度接缝使用标准 Sender。
旧 callback_seen/finished 和手工 receiver 转发只有在标准组合接管了对应完成合同后删除。
可保留一个实际解码状态对象，不把每个范围重做顶层 Task、复制整个世界或建立新状态机。

`WorldLoadingSystem` 继续拥有请求意图、required partitions、读额度/暂存字节和已完成 Read。
采用仍是主线程事务；过时 epoch/revision 的结果被丢弃并清偿，不能在 Worker 修改 ECS。
共享的不可变 World 描述/已解码分区若存在真实多消费者，按实际消费者保留，不为去 shared_ptr 增加复制。

### 5.3 正式文件范围读取

增加 `WorldFileStorageSource.hpp/.cpp`，与现有 WorldMemoryStorageSource 并列：
只实现现有 `ReadWorldStorageRange` 的文件提供者，不是存储管理器。

从测试 Disk 的真实范围读取逻辑提取有用部分，重新检查短读、偏移溢出、文件尺寸、关闭和取消：
项目装配将 `WorldStorageVolumeDescription::member_name` 冷态解析成受项目根约束的路径；
端点使用相同 ExecutionRuntime 的 blocking 资源读取指定范围，不每次加载整个 volume。
Provider 不解析 Partition，不访问 Render/Scene，WorldStorageSource 仍持有一个现有 OperationPort。

同一个 fd/stream 的 seek/read 不能被并发任务无保护共享。采用真正独立的范围读取句柄/系统调用，
实现放在该具体 provider；不因此新增一个跨模块 IO 框架。实际 provider 的打开/缓存策略由测量决定，
第一闭环不承诺 OS 原生异步 IO。文件内容变更由 bundle generation/source epoch 处理。

### 5.4 已接纳工作与资源限制

世界/资产入口对工作数、字节和结果暂存量限额；读取中间步骤复用本次流水线的额度。
不能用 Task 元数据等待 UI 消费来隐式限制 IO，也不能为了 Main 暂停而积累无限结果。

Main 暂停时，已接纳且依赖已满足的读取、解码、上传和运输终态应继续。
达到结果暂存/在途额度后停止新准入是正常背压；这不等于允许无限后台流送或无限新帧。

## 6. 独立 Asset Client 与 Main Client

### 6.1 两条 Link，一处业务权威

`RenderExecution` 创建两个现有 RenderRequester：

| Client | 唯一发送/收取线程 | 用途 |
|---|---|---|
| Main | Main | 完整帧、可见状态改变、实例/材质更新、资源移除及 Scene/View/Target 关系 |
| Asset | Asset 通信线程 | 已解码资源的非 frame-scoped 创建、上传以及对应 Response |

Asset 线程持有 Core RunLoop，通过标准 scheduler 接收有限工作。
CPU worker 用 `continues_on(assetScheduler)` 后启动 RequestSender，不能直接并发操作该 SPSC Requester。
不新增 AssetRequester 类型、AssetService 队列或第二份 AssetId→GPU handle 表。

RenderAssets 的 Main 业务目录只在结果采用时更新；上传流水线拥有这次输入和未采用结果。
若接收 Scene 已不存在而 GPU 创建成功，仍须按原连接和创建代际清偿真实句柄，不能丢弃成功结果泄漏资源。

### 6.2 当前 Command 的边界不能省略

目前 CreateTexture/CreatePatchedTexture/CreateMesh 非 frame-scoped；
CreateMaterial/UpdateMaterial、实例操作和 Texture/Mesh/Material 释放是 frame-scoped。

第一闭环保持这个准确边界：读取、Cooked 解码和 Texture/Mesh 上传后台化，Material/实例可见发布进入 Main 帧。
不能为了“所有上传都走 Asset”直接翻转 frame_scoped 标志。
若以后确需离帧创建未引用材质，必须单独证明表更新与当前帧的隔离，再修改该 Command 合同。

两个 Link 没有天然全序。Main 只有收到对应成功结果并采用后，才能在后续 FrameInput 中引用该资源。
Render 不根据资源名称猜测跨 Client 依赖，不恢复 Texture→Material 自动联动或资源保活网络。

### 6.3 唤醒与 pump

Asset loop 使用既有 IPC wake epoch 和 Core RunLoop 的 wake 回调：

1. 记录 IPC epoch。
2. 派发有限 Asset continuation，消费实际 Asset Response。
3. 检查 stop、待清偿请求和 RunLoop publisher；无工作时调用 IPC 的可唤醒等待。
4. RunLoop 发布、IPC Response、stop 均改变可观察条件并唤醒；不能检查完再无条件睡眠。

Main Requester 不能由 Asset 线程顺手消费。IPC notifyWork 本身不唤醒 GLFW：
在 RenderServer 的真实 Outbox 发布分支补一个借用的 response-available 通知，
RenderExecution 将其映射到应用 ExecutionRuntime/window wake，Main 再收自己的 Link。
Server 异常退出也要通知。不得使用 flushTo 的“结算数”作为已发布数，它还包括关闭丢弃。

该通知只是一条提示，不运输结果、不改业务对象。目标有效期覆盖 Server/发布尾栈；
Server join 后才能拆除通知。旧 UI 每帧 bindProgress→CompletionWork 扫描随消费者迁移删除。

### 6.4 借用、取消与失败

| 边界 | 合同 |
|---|---|
| 冷态绑定/发送前拒绝 | 无远端副作用，输入未转移，可重试 |
| 发送成功 | Request 可能被消费；借用 source 地址、范围、内容保持不变 |
| 收到任意正常 Response | 结束该请求的 CPU source 借用；保留业务错误/未知副作用事实 |
| Client 取消/abandon | 已发送工作仍收真实终态；不是 GPU cancel |
| 断连且缺 Response | OUTCOME_UNKNOWN；先 Server 和 Producer 清偿，后释放仍被借用的 source |
| 对象/项目关闭 | 撤销业务采用，不撤销已发生的读取、文件发布或 GPU 创建 |
| Main frame 失败 | 按 FrameCompletion.outcome/requires_resynchronization 处理，不假设整帧原子回滚 |

Request 所有权恢复不属于当前 IPC 合同。不要恢复已删除的 TypedRenderCompletion/original_request 接口。
已收到 Response 的上传 source 可以在运输层释放，不必为了等 UI 采用再保留大块字节。

## 7. Scene、完整帧和真实 FIF

### 7.1 SceneSystem 接线

新增正式 `engine/scene/builtin_systems/render`，遵循当前 SceneSystemRegistration/Installer，
不恢复旧 RenderFeatureSceneBindings 或旧 ECS Render bridge。

| Scene 阶段 | RenderSystem 动作 |
|---|---|
| MAINTENANCE | 采用已到达结果，核对 Entity generation、Scene/revision；结构改变在现有 EcsCommandBuffer 安全点执行 |
| SYNCHRONIZATION | 原 TransformSystem 更新世界矩阵及结构命令 |
| STABLE | RenderSystem 在所有同步系统完成后借用矩阵，准备变化身份和 Camera 参数，不再计算第二份 Transform |
| PUBLICATION | 向共同 FrameInput 追加本 Scene 的 K 项变化及 Attachment 参数；不自行 SubmitFrame |

观察者只记意图/脏项，连接时折入存量；on_update 写入走 patch/replace；
on_destroy 在字段尚可读时提取句柄。不能把历史 AGENTS 示例路径当成当前存在的系统恢复回来。

Camera 由 Engine 转成专业 Feature 参数，通用 Render View/Compiler 不认识 Camera。
材质按真实程序表/mesh/submesh 分组；参数/Transform 改动不重编图。
Scene 和 UI 通过资源/端口协作，不通过另一个 Feature 的实现对象取状态。

### 7.2 一帧只提交一次

```text
Main 收取完成/窗口事件
  → beginFrame 取得 L+1 中一个空输入载体
  → 推进本次 Simulation 与 Scene 同步
  → 所有可见 Scene 追加 Command groups
  → 捕获独立 UI Scene 数据并追加
  → SubmitFrame（一次）
  → Render 在真正可复用的 GPU frame slot 上准备、录制与提交
  → FrameCompletion 返还 CPU 输入借用
```

没有载体时仍处理输入、完成、关闭和必要维护；不再生成一个没有容量存放的游戏帧。
多个 Scene/窗口共享本应用帧源，不各自建立领先帧计数。
Game pause 可停止 Simulation，但仍允许 UI 帧；Main 真暂停则不会凭空产生无限新帧。

L+1 CPU 载体、GPU FIF slot、SwapChain image index 是三种身份，不能取同一个模数混用。
FrameCompletion 表示输入消费和提交结果，不证明 GPU 完成或已经显示。
GPU slice/descriptor/command allocator 仍按真实 Queue 前沿复用。

CPU 数据的权威保持清楚：Scene 拥有活动世界；FrameInput 冻结本批 K 项，Transform 可直接在载体内由 double 转成 GPU float；
Render 的已发布数据与逐 slot 脏状态用于修复尚未更新的 FIF slice。
不为了减少一次必要快照而借用下一次 Simulation 会改写的内存，也不复制完整世界。
revision n 仅确认 n，等待期间的新变化不能被旧回复清掉。

结构性帧内控制也可用现有 RequestSender：Requester 暂存真实命令输入，下一次 beginFrame 纳入完整帧，
Response 仍交给原 stdexec Operation。它不创建第二套结果队列或操作 Owner。
创建、移除和替换可以在项目关闭时由维护帧清偿；普通热帧批量数据仍直接 append，避免逐项 Task。
Attach 可携带同 Scene/View 的待替换 AttachmentId；目的 Target 必须空闲或由该 Attachment 写入，候选全部准备成功后才替换；
失败保留旧实例，不能先 detach 再赌新 attach 成功。

`RenderRequester::buildingFrame()` 是当前唯一正在构造的 Main 帧借用；Scene 从这里追加，
不再由每个 SceneRenderInputs 保存/设置第二份帧指针。submit/release 结束该借用。
Requester 根据收到的 FrameCompletion 更新 resynchronization 版本，各 Scene 只重发自己的权威值。

FrameInput 可以把平凡数组冻结到自身的分配区，直到真实 FrameCompletion 后再释放；
Scene 或 Pane 提前销毁不会使这些 span 悬空。L+1 载体共用标准 PMR 块复用，
每个载体只有自己的临时数据范围，不增加 Scope、资源身份或任务 Owner。
已有应用级稳定数组仍可直接借用；不强制多复制一次。
UI 捕获可直接写入该分配区，避免先复制 ImDrawList，再复制中立数组。
块缓存只影响内存复用，不是帧数据总量限制；大于缓存阈值的工作量必须单独记录分配成本。

### 7.3 渲染资产的完整流程

```text
Main 资产需求/版本
  → blocking：VFS/read range
  → cpu：Cooked 解码/世界解码
  → Asset：Texture/Mesh 上传请求
  → Render：Storage Sender → stdexec 上传 → 真实资源发布
  → Asset：运输 Response，释放已结束的 source 借用
  → Main：RenderAssets 采用 → FrameInput 创建/更新 Material 与 Instance
  → Render Graph：通过明确端口渲染 Scene
```

资源依赖归 Client；例如释放 Texture 前更新引用材质的顺序由 RenderAssets/实际业务发起方决定。
Render 不替上层保存旧 Texture 版本或自动维护跨 Storage 关系。
应用期固定已注册的扩展代码；项目关闭不等于 DLL 可卸载，热卸载不纳入本轮。

## 8. 窗口输出与 Target

### 8.1 SurfaceTarget

新增 Render 私有 `SurfaceTarget.hpp/.cpp`，拥有一个 NRI SwapChain 及其实际呈现同步对象。
与 OffscreenTarget 共用 TargetId/FrameExecution/SubmissionQueue，不增加呈现 Manager 或线程。
创建描述包含明确平台窗口借用、extent、格式和呈现策略；Renderer 不调用 LuxWindow/UI 对象。

每个有效窗口的顺序：

```text
有完整输入 + 可用 GPU frame slot + 非零有效窗口
  → AcquireNextTexture
  → 绑定本次取得的 Target image
  → Graph 首次使用等待 acquire；末次写入转 PRESENT
  → 最终提交 signal release
  → QueuePresent
```

核对锁定 NRI v180 的 NRISwapChain.h/SwapChainVK 实现后确定 semaphore 复用，
不能以普通 GPU Fence 完成代替 WSI 已归还 image 的证明。
Resize/minimize/out-of-date 在 Render 安全点处理，候选建立失败保留可用旧状态或准确不可呈现状态；
Window 必须活到 Surface 完全清偿。

`WaitForPresent` 是可选呈现节奏能力，要求设备支持 waitable swapchain；
NRI 说明要求从首帧开始、输入采样之前调用。它不是 CPU 输入载体、GPU FIF 或 IPC 等待的替代品。
第一闭环不强制启用 WAITABLE，不把阻塞的 NRI 调用放入共享 CPU 池。
如果启用，必须先证明双线程输入时序、可退出等待与多窗口公平性；不能把 RenderThread 上提交后的 wait
宣称为已经满足 Main 输入采样前的延迟控制。Acquire 本身也可能阻塞，真实窗口测试决定其可接受性。

### 8.2 Scene 输出进入 UI

OffscreenTarget 增加明确的采样用途和 Target 图像引用。UI 导入真实物理输出，
不要通过裸 descriptor index、CPU Readback 或隐藏上一帧资源传图。

当前 Target producer 不能仅靠 imported 摘要自动发现：需把输出写入、UI 读取、实际 image/epoch/subresource
及最终状态一起交给现有 FrameExecution 依赖构建。复用已有依赖缓存失效机制，不每帧重编 RenderGraph。

第一闭环采用同一 sealed frame 内的 Scene producer→UI consumer：
UI 最后采样也是该 Target 的真实使用，不能只等 Scene 完成就覆盖它。
未渲染过的输出不能采样未初始化内容；Scene 暂停但视口可见时可不推进 Simulation、仍渲染该 View。
独立频率“永远显示上一张图”需要明确的输出版本/History 合同，暂不暗中引入。

Resize/关闭先停止未来引用并更新关联；旧 image、descriptor 和相关帧物理数据走唯一 cleanup_scope_。
不能用全局 Device idle 或新增 CPU Fence Wait 作为每次 UI 采样的常规接线。

具体输入由 `AttachmentRequest::target_images` 指定图中的资源键和 `TargetId`。图中该资源必须是
带 `SAMPLED` 初始状态的 imported image；动态尺寸使用 `ImageExtent::source = IMPORTED`，
而不是把消费者窗口的尺寸误用为 Scene 输出尺寸。格式、Mip、Layer 和 Sample 数仍是编译合同。
`FIXED` 和 `TARGET` 分别保持固定尺寸、按当前输出 Target 缩放的含义。

Target 只保存非拥有的消费者 Attachment 索引。Attach 拒绝循环；执行列表仅在关系或启用状态改变时
按生产者顺序整理，随后复用原有物理访问摘要建立批次依赖。生产者未附着或被停用时，本轮也不执行
依赖它的消费者，不能悄悄采样未初始化内容或上一帧内容。Client 应先解除消费者，再删除输入 Target；
仍有消费者时 RemoveTarget 明确失败。Resize 先准备所有受影响实例和 Descriptor，再统一采用，
不会在旧 Descriptor 仍引用旧图像时提前销毁它。

Surface 的 Acquire semaphore 按可复用 GPU slot 管理，Present semaphore 按交换链 image 管理。
RemoveTarget 的 Response 才归还平台窗口借用。冷态关闭/同窗口重建在唯一 cleanup_scope_ 内等待
已提交 Queue 前沿，再清偿实际 presentation queue；Acquire 后尚未提交便失败时，也先消费其信号。
此处 QueueWaitIdle 只用于 WSI 生命周期边界，不进入稳定帧；普通帧仍是非阻塞完成推进和真实 FIF。

## 9. 独立 UI Scene 与 ImGui Feature

### 9.1 保留独立 Scene，删除空 World 运输

保留 `EditorUiScene` 名称，但重写为真正的 Render Scene/View/Effect 组合。
删除其仅为旧 RenderSystem 运送 UI 帧而构建的空 World、空 Simulation 和 UiFrame ECS 组件。

`publishFrame()` 改为 `appendFrameCommands(FrameInput&)`：
只追加本次 UI 输入，由应用统一 SubmitFrame，不能 UI 和项目各提交一帧。
保留 hasWritableFrame/capture 的背压语义，复用 CPU 数组容量。
删除 use_count()==1 判断、旧 RenderFrame/submission.complete()、bindProgress 和专用完成扫描。

UI 场景属于窗口/应用，可跨项目存活；项目 Scene 关闭只解除其视口图像引用。
Root、Pane、输入路由、DPI 和 LuxObject Signal 仍由 Editor/UI 线程管理。

### 9.2 中立绘制数据与实际必要快照

新增资源描述域 `UiDraw.hpp`，公开纯值格式：Position/UV/Color 顶点、索引宽度/范围、
元素数、base vertex/index offset、clip、图像 token、DisplayPos/DisplaySize/FramebufferScale。
它不包含 ImDrawData、UI 对象、RenderStorage 或 NRI 类型。

`ui::DrawData::capture` 从 ImGui 活动数组直接生成可复用拥有型快照；
删除“先复制完整 ImDrawList、再复制等价 Render 数组”的双重运输。
ImGui 下一帧会覆盖原数组，因此必要的一次冻结不能声称可以无条件消除。
移除 VulkanRenderer friend 与旧 RTextureHandle；ImageElement 使用中立 token。

应用在捕获边界将 token 解析为本帧 Texture/Target 引用表，GPU draw 使用紧凑索引。
这只是帧数据，不新增全局图片 Registry。无效 token 在 Client 捕获边界决定失败/占位语义，
不让每个 draw 在 GPU 录制层查业务名称。

任意 ImGui UserCallback 不跨线程执行。已知 ResetRenderState 规范化为实际绘制状态需求，
其它回调明确拒绝；不把函数指针保存到 RenderGraph。

### 9.3 Feature、Storage 与 Raster

采用明确名称 `UiRasterFeature`，实现放 Render 私有 features 区域，公开描述在 include；
它依赖现有纹理声明和本次帧实际 UI 数据，不依赖另一 Feature 的实现。
UI 帧几何由 Attachment/FrameResources 保存；没有独立资源身份需求时不新建 UiStorage。

增加实际通用能力：

- `DrawIndexedList` 描述固定顶点/索引布局；`IndexedDrawRange` 保存本帧 index 范围、base vertex、
  scissor 和已解析图像/参数索引。数据对象不持 Queue、不自行提交。
- `RenderGraphInstance::FrameResources` 保存对应 slot 的稳定 draw records 与实际几何缓冲。
  更新复用现有 stdexec 上传/帧准备通路，不增加 ImGui 专用 uploader 或 per-window frame scope。
- `RenderExecutor::recordBatch` 在一个 Raster Pass 中执行多条 clip/texture/indexed draw；
  不为每个 ImDrawCmd 构建一个 Graph Pass，不逐帧编译 Pipeline。
- `GraphicsState` 增加明确 ColorBlendState，进入 Pipeline 身份、冷态校验和 NRI 创建。
  Shader.cpp 不根据 Feature 名硬编码 Alpha。
- 字体 atlas/普通图片使用现有 SampledTexture2D 上传；Scene 输出通过 Target 图像端口。
  Render 不创建 ImGui Context，不加载字体或访问 Pane。

从当前修改版 ImGui 的 imgui_impl_vulkan.cpp 复核并迁移 DisplayPos/FramebufferScale 投影、
clip clamp、IdxOffset/VtxOffset、Alpha Blend 等成熟算法。
只迁算法，不链接 Vulkan backend、不移植它的 Device/DescriptorPool/Fence/独立帧寿命设施。
同时验证直通 Alpha/预乘策略与 [色彩合同](color-space.md)，最终显示编码只发生一次。

## 10. 主循环、关闭与错误采用

### 10.1 主循环

`LuxEngine::pumpOnce` 拆为可直接阅读的阶段，函数名表达动作：

1. 收取原生事件、Main Response、通用 Main continuation 和监控变化。
2. 派发 Object 固定批，采用项目/窗口意图；不在完成回调递归 driveFrame。
3. 有输入载体时推进 Scene，冻结项目输入与 UI 输入。
4. 统一提交完整帧；无载体时只做必要维护，不忙重试或继续积累绘制数据。
5. 推进业务退休；空闲用可唤醒等待。Execution/Object/Render stop 和回复都能唤醒窗口。

`EditorWindow` 的回调只发布尺寸/输入意图，不直接操作 Server。
与引擎无关的 headless 应用复用相同执行资源与完成合同，不依赖 GLFW。

### 10.2 项目关闭

先从 Root 撤销该项目 UI，再请求该项目实际拥有的 SceneRuntime 实例退休。Scene 系统的析构把必要的 Render 关闭事务
登记到已有项目 TaskScope 后，再关闭该 Scope 的新准入并请求停止；不能先关准入再要求析构补发释放。
已接纳的 Render 事务保持真实 Response/句柄清偿，不因项目停止而跳过清理。
项目把实际 SceneInstanceLease 交给 `EditorContext::adoptScene()`。close 仅请求退休，
应用保留有 Scene 借用的关闭 Context 并继续 SceneRuntime/维护帧；finishClose 在退休回执完成后
关闭 TaskScope 准入，等待它的已接纳清偿结束。它只保存真实 lease/回执，不建立另一份 Scene 目录。
Scene 退休完成加项目 TaskScope.settled() 才允许移除其输出 Target 和销毁相关能力提供者。
这仍是现有项目任务组，不是新增 GPU 清理 Scope。
Pane/EditorContext 可先撤销 UI 采用，但已经发送的上传输入、文件 provider、代码 pin 和结果清偿不能随它们销毁。
没有 Scene 借用的 Context 保留原来的非阻塞销毁合同；已接纳任务自行拥有输入和代码。
项目关闭不停止整个 ExecutionRuntime，不 join RenderThread，不卸载仍有回调/Handler 的 DLL。

成功创建但已经无人采用的资源走真实释放命令；该清偿属于应用仍存活的 RenderAssets/协议流水线。
若释放是 frame-scoped，放入关闭维护帧，不等待一个已经销毁的 Scene 再生产普通帧。

### 10.3 应用退出

```text
关闭业务准入、取消项目任务、停止新普通帧
  → Main/Asset/Render 继续推进，清偿已接纳 IO、CPU、上传与关闭维护帧
  → 请求 RenderServer 停止；RenderThread 继续推进 GPU、RunLoop、Outbox、cleanup_scope_
  → RenderThread join，确认其借用和 Producer 尾栈结束
  → 各 Requester 在所属线程 settleClosed，缺失结果报告 Unknown
  → Asset continuation/发布者清偿，Asset 线程 join
  → 销毁渲染设施；再停止并 join 共享 Runtime 的其余任务/IO/CPU/Timer
  → 收尾 Object，释放已经没有 Surface 使用的 Window，销毁 EngineContext
```

不能在共享 CPU/IO 资源已停止后，要求 Render/Asset 调度最后一次准备或清偿。
不能在 Main 阻塞 sync_wait 一个必须靠 Main/Object 队列继续执行的关闭任务。
Scope join、协议借用结束、Producer 退栈、GPU 完成、WSI 归还分别按真实依赖确认，
不新增一个“都安全了”的冗余生命周期对象。

现有 `HostResources::drainAcceptedWork` 改为上述显式流程，
同时将该私有聚合重命名为 `ApplicationResources`，表达应用组合成员而非笼统 Host。
关闭期必要等待允许存在；不进入稳定渲染热路径。

## 11. 类、函数、文件与 target 清单

表中路径相对仓库。已有头/源同职责时保持同名；新公开头安装，组合内部实现留 pinclude，
项目扩展面确有消费者才放 sinclude。模板可以只有头，不制造空 cpp。

### 11.1 Core Execution 与既有异步消费者

| 文件或类型 | 修改/删除 |
|---|---|
| `modules/core/execution/include/lux/engine/core/execution/TaskScope.hpp` | 保留有限任务分组；迁单一工厂 submit，删除三参数隐式采用重载 |
| 同目录 `Task.hpp` | 删除 TaskOperation/TTaskOperation、通用 connected owner、deliver；保留 TaskId/TaskReporter/实际取消及错误语义 |
| `modules/core/execution/pinclude/lux/engine/core/execution/detail/TaskState.hpp` | TaskRecord 不拥有 operation；清理结果运输字段；TaskRuntime 保留实际监控目录和接纳，容纳标准执行 Scope |
| `modules/core/execution/src/Task.cpp` | 迁 admit/start/complete/collect/release；执行清偿与监控派发分开，删除业务 deliveries 路径 |
| `ExecutionRuntime.hpp/.cpp` | 单一 CPU 池不重做；补 Main Scheduler/明确派发/settled；迁 submit、关闭顺序和旧 completed API |
| `CompletionWork.hpp`；其现位于 ExecutionRuntime.cpp 的实现 | 保留合并通知语义，迁同一 RunLoop；实现归独立 `src/CompletionWork.cpp`，删除旧 ready_completion/completion_batch 运输 |
| `RunLoop.hpp/.cpp` | 复用现有 moodycamel、批处理和发布尾栈；只补确实需要的接口/注释，不暴露 Work* 给业务 |
| `modules/core/task/TaskExecutor.hpp/.cpp` 与内部整图 Sender | KEEP：共享池、DAG、affinity、workspace；不重写成逐节点 spawn |
| `engine/process/object_execution/.../ObjectScheduler.hpp` | 保留代际/对象借用；声明清楚 value 与拒绝的线程差异，删除消费端重复交付 |
| `engine/scene/composition/src/SceneRuntime.cpp` | 迁完成派发与定时通知，保留 Scene 时钟/驱动；不增加另一主循环 |
| `engine/scene/integration/script/src/ScriptRuntimeSystem.cpp` | 迁 delay 的 Task 完成转发，保留脚本恢复安全点 |
| `engine/scene/scripting/assets/ScriptAssetAccess.hpp/.cpp` | 按实际 include/src 路径迁 submit/完成，保留 VM 身份、准入及代码寿命 |
| Core/Scene/Script/Editor 的上述 API 测试与安装消费者 | 同模块迁消费者；保留同步完成、取消、晚析构、无主线程中转等行为断言，不保留兼容重载 |

TaskScope 内部迁移是一个完整模块，不能先删除 API 留一半消费者。
新 Scope 不负责 GPU 对象清理；现有 Render Scope 不合并进 TaskRuntime 任务目录。

### 11.2 IO、解码与世界流送

| 文件或类型 | 修改/删除 |
|---|---|
| `engine/process/asset_loading/src/VfsAssetReadEndpoint.cpp` | 改 Impl::submit/Request::finish/mapExecutionError；删除主线程完成依赖和无真实生产者的 OOM 容量映射 |
| 对应 `include/.../VfsAssetReadEndpoint.hpp` | 明确准入、线程、销毁和借用；只保留实际依赖 |
| `include/.../AssetLoadSender.hpp` | 删除 TAssetReadReceiver/等价手工转发 TState，标准组合表达读取→CPU→解码；保留业务错误区分 |
| `AssetReadOverlay.cpp`、AssetVfs/AssetProvider | KEEP 实际 fallback 与 mount 快照；不复制新 VFS |
| `engine/process/world_loading/include/.../WorldPartitionLoadSender.hpp` | 增加明确 CpuScheduler 输入，迁逐范围完成 |
| `src/WorldPartitionLoad.cpp` | 拆实际解码推进与调度；删除手工 callback 中直接解码 |
| `WorldStorageSource` 的定义当前混在上述 cpp | 迁为匹配的 `src/WorldStorageSource.cpp`，不改变源身份合同 |
| 新 `include/.../WorldFileStorageSource.hpp`、`src/WorldFileStorageSource.cpp` | 正式有限文件 range provider，使用同一 blocking 资源 |
| `WorldMemoryStorageSource.hpp/.cpp` | KEEP 同步内存提供者，作为调度/重入有效消费者 |
| `engine/domain/world/storage/src/WorldPartitionDecoder.cpp` | KEEP 解码算法；无必要不改格式/重写 Parser |
| `engine/scene/builtin_systems/world_loading/src/WorldLoadingSystem.cpp` 及 public services | 传入 CPU scheduler；迁 Task completion，维护 source epoch、需求 revision、容量与主阶段采用 |
| 世界加载测试中的 Disk | 正式 provider 替代重复实现；测试保留延迟、故障、短读等必要控制 |

### 11.3 Engine 装配、Client 与 Scene

| 文件或类型 | 修改/删除 |
|---|---|
| `engine/context/include/lux/engine/EngineContext.hpp` | 删除 RenderContext 前置声明、renderContext()、rendering_、EngineRenderAccess；不增加渲染隐式初始化 |
| `engine/context/src/EngineContext.cpp` | 保留唯一执行资源、资产与 Scene；生命周期注释与实际合同一致 |
| 新 `engine/rendering/include/lux/engine/rendering/RenderExecution.hpp`、`src/RenderExecution.cpp` | 实际 Server/两 Link/协议线程装配；公开 execution、mainClient、assetScheduler 等窄接口，不暴露 Impl/Device |
| 现有 `RenderRequester.hpp/.cpp` | KEEP typed Sender/有限载体；补实际线程与关闭通知消费，不增加 operation Owner |
| 新 `RenderAssets.hpp/.cpp` | 明确资产需求/版本、有限上传流水线及 Main 采用；不是 GPU 内存管理器 |
| 新 `RenderDiagnostics.hpp/.cpp` | 一处将结构化 RenderErrorSink 映射到宿主诊断；Render 仍不链接 lux::log |
| 新 `engine/scene/builtin_systems/render/include/lux/engine/scene/RenderSystem.hpp`、`src/RenderSystem.cpp` | 正式 SceneSystem 注册与增量发布；私有 Asset/Entity 关联留 pinclude，不在组件头 include 系统 |
| 新同模块 `SceneRenderSchema.hpp/.cpp` | 实际渲染组件/反射与窄数据合同，沿当前 schema 生成机制 |
| `engine/scene/plugins/BuiltinSceneRenderExports.cpp` | 迁现行 SceneSystem/component 导出；删除旧 RenderScenePluginExports/builtinRenderFeatureSceneBindings 导出 |
| Engine/Scene 的 Render 旧 README/配置消费者 | 一次迁当前 descriptor/ID/FrameInput；不复制缺失旧头或增加别名 |

### 11.4 Render 窗口、Raster 与 UI

| 文件或类型 | 修改/删除 |
|---|---|
| `modules/function/render/include/lux/engine/render/core/Target.hpp`、`src/core/Target.cpp` | 实际 Surface 描述、采样用途和窗口借用 |
| `include/.../commands/Commands.hpp`、`src/runtime/RenderServerImpl.cpp`、`Scene.cpp` | Target 创建/resize/关闭与帧顺序；保持显式 Response 错误和真实异步清偿 |
| 新 `pinclude/lux/engine/render/execution/SurfaceTarget.hpp`、`src/execution/SurfaceTarget.cpp` | NRI SwapChain 与呈现对象，单一设备/队列 |
| `sinclude/.../execution/OffscreenTarget.hpp`、`src/execution/OffscreenTarget.cpp` | 采样使用及 Target 输出资源信息 |
| `FrameExecution.hpp/.cpp`、`RenderGraphInstance.hpp/.cpp`、SubmissionQueue | Target producer/read 摘要、Barrier、WSI 同步、真实 FIF 与最后使用 |
| `include/.../runtime/RenderServer.hpp`、`pinclude/.../runtime/ResponseOutbox.hpp`、对应 src | 借用回复可用通知；只从真实发布/终止边界唤醒，不把结算统计当发布事实 |
| `include/.../graph/Draw.hpp`、GraphCompiler、`src/graph/Shader.cpp` | DrawIndexedList/IndexedDrawRange、ColorBlendState、冷态校验/Pipeline 身份 |
| `sinclude/.../execution/RenderExecutor.hpp`、`src/execution/RenderExecutor.cpp` | 一 Pass 多 draw 的裁剪/图像选择/索引偏移，沿现有状态缓存录制 |
| 新 `include/.../features/UiRasterFeature.hpp`、`src/features/UiRasterFeature.cpp`；现有 `pinclude/.../features/BuiltinFeatures.hpp` | public descriptor 与源内实现分离，私有登记声明接已有 tuple；不增加与公开头同 include 路径的私有头 |
| 新 `modules/resource/description/include/lux/engine/description/UiDraw.hpp` | 中立 CPU/GPU 绘制值合同；若只有结构体不加空源文件 |
| `modules/function/ui/include/lux/engine/ui/DrawData.hpp`、`src/DrawData.cpp` | 直接冻结中立数组，删除 ImDrawList 运输副本和 VulkanRenderer friend |
| `ImageElement.hpp/.cpp`、`sinclude/.../detail/ImageEncoding.hpp` | 中立 image token；删除旧 RTextureHandle 编码 |
| `editor/app/pinclude/.../detail/EditorUiScene.hpp`、`src/EditorUiScene.cpp` | 保留类型，重写为独立 Render Scene；publishFrame→appendFrameCommands；删除空 World/Simulation |
| `editor/app/.../UiRenderSyncStage.hpp/.cpp`、`detail/UiFrame.hpp` | 新 UI 路径闭环后删除，不留下旧运输接口 |
| `editor/app/src/LuxEngine.cpp`、`ProjectPreparation.hpp/.cpp` | 共享执行资源、项目准备组合、统一帧发布、显式关闭 |
| `editor/context/EditorContext.hpp/.cpp`、EditorComposition | 借实际设施，迁 Task API；ContextExtensions 仍一次发布 |
| `editor/window/EditorWindow.hpp/.cpp` | 尺寸 revision、唯一输入与 wake 接线，窗口回调不直接录制或销毁 GPU |

上表没有列出的 Render 热路径默认不改。不能在不同可见层建立相同逻辑 include 路径而依赖搜索顺序选择不同类型。
不为了文件配对增加转发头、导出具体内置 Storage，或让 public descriptor 包含实现。

### 11.5 CMake、SDK 与依赖

| target/配置 | 迁移 |
|---|---|
| `core_execution` | 只增加/调整真实源文件，仍依赖锁定 stdexec 与既有 ConcurrentQueue；消除旧完成源清单 |
| `engine_rendering`（已存在） | 扩展 RenderExecution/RenderAssets/Diagnostics，链接实际 Core/资产依赖；安装实际扩展头 |
| `engine_context` | 删除 `if(TARGET render_runtime) add_subdirectory(rendering)`；保持 headless 闭包 |
| `process_asset_loading/process_world_loading` | 新 Sender/正式文件 provider 依赖；保持 process→domain/modules |
| 新 `scene_render` | SceneSystem 的真实实现，替换缺失 render 子目录条件；不创建另一个 Renderer |
| `engine/scene/CMakeLists.txt`、Scene 插件 | 使用 scene_render 和现行导出；删除旧 render_runtime 条件与无消费者渲染插件 ABI |
| `modules/function/ui/CMakeLists.txt` | 去 render_client、imgui vulkan 与旧 rendering 子目录；CPU UI 依赖中立 UiDraw |
| Render 唯一库 | 增加 Surface/UI 专业实现，仍不依赖 engine/editor/ImGui |
| `editor/CMakeLists.txt` | 用真实 engine_rendering/scene_render 替换 engine_context_render/project_plugin_rendering 及旧找包；同次迁所有测试和安装消费者 |
| module README/SDK 包 | 更新使用流程、安装头与依赖；不留过时 shim/旧 package 冒充兼容 |

不得仅注释失效 target 依赖使 Editor 链接成功。每个删项必须对应真实消费者迁移；
若还需要插件装配，使用现有项目插件目录和 Scene 导出，不重新建一个空的 project_plugin_rendering。

## 12. 分大模块实施与集中验收

顺序为 A→B→C→D→E→F。每个大模块完成接口、实现、消费者和错误路径后集中验证，
不按单个函数/类型反复运行全矩阵。必要编译诊断不受此限制。

| 模块 | 完整成果 | 主要门禁 |
|---|---|---|
| A：统一有限任务与完成交付 | TaskScope/Runtime 标准 Scope、显式业务采用、同一 Main 完成通路；Editor/Scene/Script API 消费者全部迁完 | 同步完成/start 重入、TaskId 取消/历史、组析构后工作存活、模块晚析构、Main 不消费时终态及容量正确、无 connected owner 回迁 |
| B：IO 与解码 | VFS 直接运输完成、标准 AssetLoadSender、World 每次读取后 CPU 调度、正式文件范围 provider | 主线程暂停时 accepted read/decode 前进；内存同步/文件异步/短读/取消/endpoint 关闭；source epoch/revision 与 staged bytes |
| C：Engine 双 Client 与 Scene | RenderExecution、独立 Asset Link、RenderAssets、RenderSystem、完整帧 | 一个 CPU 池；固定 SPSC 线程；两个相同数值 RequestId 不串 Link；上传借用、额度、跨 Link 发布、两 Scene、关闭维护帧 |
| D：窗口与 Target | 真 Surface、输出采样、明确图间依赖及关闭 | 真实窗口/多窗口、resize/minimize/out-of-date、FIF/WSI 同步、GPU Validation/SyncValidation、项目暂停/恢复 |
| E：UI 完整闭环 | 独立 Render UI Scene、UiRasterFeature、中立快照、IndexedList/Blend、项目视口合成 | clip/DPI/透明/多纹理/索引宽度/大网格、字体上传、两窗口、项目切换、Pane 删除、完整帧与 CPU 借用 |
| F：产品与 SDK 收口 | 删除旧接线、正式安装/迁移 prefix、更新使用说明、真实 Editor 启停 | Root EDITOR 实配、all/第二轮无工作、公共头/安装消费者、ASan/GPU、关闭与磁盘清理 |

A 的 OperationState 迁移先做同一模块内的实际生命周期证明，再迁全部消费者；
验证失败不能留两套 submit/队列作为终态，也不能扩大成新 TaskManager。
D 的呈现等待和 Target 跨图使用必须实测，不能以离屏图像测试替代。
E 的图像正确性包括像素/裁剪/混合与人工查看实际 Editor，不以“进程退出码为零”替代显示验收。

最终使用同一源码树/既有增量构建目录；独立验收绑定精确 clean commit，只保留本轮必要验证副本，
不重复积累 SDK、build、PDB 和依赖缓存。源码、依赖与必要日志可追溯即可。

## 13. 质量、成本与判定

### 13.1 架构与代码审查

- 对每个新增长期类型列明实际成员、消费者及被删除的旧职责；能用现有对象的方法就不再加转发层。
- 公共接口注释必须交代线程、借用期限、失败和取消，不用大段注释解释不合理结构。
- 按项目 4 空格、120 列、函数间空行和命名约定；头/源职责及文件名对应。
- 数据不保存“执行自己”的 Queue/Runtime；动作属于 Requester、Storage、Executor 等实际执行者。
- 边界验证后使用 ID/紧凑索引；不把 Client 校验取消误当成 Server 可以无条件信任外部输入。
- 不将 callback 层数直接等同于运行时成本：标准组合可能内联，队列切换/分配/等待必须分开统计。
- KEEP 实际取消、代际、FIF 修复和 GPU/WSI 同步；删除重复观察、无实际生产者的状态与多次同值检查。

### 13.2 成本预算的落实

| 路径 | 设计约束与需要记录的事实 |
|---|---|
| TaskGraph | 保留 O(V+E)，不逐节点顶层 spawn，不增加全图阶段 barrier |
| 普通同步 Command | 不进入 TaskScope，不新增分配或线程切换 |
| 有限 IO/上传任务 | 允许标准 Scope 必需分配；记录次数/累计字节/状态大小，禁止每层再分配 operation/结果信封 |
| 读取→解码→上传 | 只有 blocking→cpu→asset 的必要切换；最终业务采用再到 Main；无 Main 中途跳板 |
| Main 完成 | 按实际就绪事件派发；监控不逐帧 taskInfos 全量复制 |
| Scene 更新 | 随 K/脏范围和实际关联变化；不复制全世界，不重复 Transform 权威 |
| UI | 必要的一次 CPU 冻结，容量复用；GPU buffer 按真实 FIF 复用；一 Pass 多 draw |
| 内存与背压 | L+1、Asset 在途、IO 字节、解码暂存各有实际含义；不以无限积累掩盖吞吐 |
| 关闭 | 不持队列锁执行回调，不从回调中递归 pump，不提前销毁 producer/代码模块 |

先正确闭环再做专项性能。记录请求端到端、IO等待/执行、CPU解码、Asset发送/回复、Main采用、
Frame/Record/Submit、堆分配、切换次数和峰值；采样与正式时延结果分开。
没有同工作量对照就不宣称更快；超既有 5% 参考线或未归因项仍为 PARTIAL。

### 13.3 最终验证与交付边界

- Windows RelWithDebInfo、Debug、MSVC ASan；C++20、无 RTTI，遵守既有异常边界。
- 必须全量 `all -j 4 -- -k 0`；CMake 改动后第二轮无工作；构建与实机测试不并发。
- 保留 Runtime/IPC/Core/Task/World/Script/Render/Editor 的适用行为断言；
  删除旧接口测试前列出有效不变量的迁移落点。
- 验证共享 CPU 与阻塞 IO 的实际线程归属；线程数量没有新增私有计算池。
- 三 Queue、真实 FIF、stdexec 上传、唯一 cleanup_scope_、Unknown/停止、跨 DLL 及 publisher 尾栈回归。
- 真窗口和真实 UI/视口、项目切换、后台上传、主线程暂停、应用关闭均须覆盖。
- SDK/headless/CPU UI 的 source/include/link/install 闭包真实成立；公共头按工程规定同步三个 prefix。
  Android 头同步不表示 Android 构建通过；未执行平台仍 NOT_RUN。
- 保护 Legacy、用户修改及既有判定；不因旧报告删除或测试改名回写资格。

分别报告执行架构、Render/产品功能、GPU/寿命安全、性能、SDK/产品闭包。
阶段材料和原始输出外置，本文件只维护架构与使用合同，不变成历史施工账本。
实现按模块提交，独立验收后才安全推送；本文的目标形状不代替实际交付结果。

## 14. 实际接入接口

- 应用启动以 `RenderExecution::create(config, execution)` 组合 Server/Main/Asset；
  Editor 的 `LuxEngine::rendering()` 与 `renderAssets()` 借用同一设施。
- EditorComposition 的 ContextExtensions 发布这两个现存对象；它们不变成 Context 的第二份 Owner。
  `EditorContext::prepareAssetReads()` 提供项目 VFS 端点，使用同一 ExecutionRuntime 的阻塞资源。
  SceneRenderServices 的 source 显式传入此端点，RenderAssets 每次 acquire 捕获它，
  不通过全局“当前项目”选择器路由。调用方用不同 asset_revision 区分来源代次。
- SceneRenderInputs 只保存 Target/Viewport/Camera 相关输入，不拥有完整帧。
  Scene 在 Requester 正在构建的 carrier 中直接写 K 项变化；UI 随后捕获并追加，应用提交一次。
- `LuxEngine::setUiImages(vector<UiTargetImage>)` 建立 UI token 到 sampled offscreen Target 的冷态关系。
  移除图像后，要等 uiImagesApplied，再移除原 Target。Target 的创建/释放仍由实际视口消费者负责。
- UiRasterFeature 的颜色运算为线性 BT.709 + straight alpha。SRGB 格式由采样器解码，
  UNORM/浮点输入必须已经是线性值；ToneMap 后的 LDR 视口宜使用 SRGB Target，
  不能把已经编码的 UNORM 值假装线性输入而再次编码。UI 不运行 ACES。
- 当前 ui::Root 的字体 atlas 在创建后不可变，因此 Editor 在初始化时上传一次；没有运行期字体重建接口。
  通用 Texture 替换仍使用原上传合同，不能把它当作已实现的动态字体更新。预乘 alpha 输入也不是当前 UI 合同。
- 首次窗口初始化时如已经最小化，延后创建 SwapChain；恢复事件才触发创建。
  Resize 事务先登记，再判断 outputReady，不能把新尺寸几何送入旧 Target。
- `UiRasterFeature` 支持本轮 CPU UI 的直通绘制数据；Context/Pane 或 ImGui 指针不进入 Render。
  具体 Scene 编辑工具、动画/LOD 选择、编辑器 picking 不在此次接线闭环内。

Main 等待还必须覆盖 RunLoop 发布尾部：节点已被消费而 post 尚未退出唤醒回调时，
hasReadyWork 仍为真。发布返回的序号与活跃计数位于同一原子字，wakeEpoch 包含这次返回，
防止在“已消费、尚未 settled”与进入等待之间丢失最后一次变化；等待方在检查业务条件前取得 epoch。
没有第二套完成队列或新的 operation Owner。
