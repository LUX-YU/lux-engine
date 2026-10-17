# C1–C5 实施与独立验收

本次范围仅为 Handler/CommandRegistry、IPC 路由、ResponseOutbox、CreateEffect Sender 和单线程事件循环。Graph/MaterialGraph、Core Execution、Native Copy/Queue/退休算法未改动。性能未归因项仍为 **PARTIAL**，没有整体无保留 PASS。

## 提交及证据

- 起点：`f6a29b502241eb15da420c0e55b5edef33ef77fc`。
- 生产 I：`bdf216206afbfa005c20a4c58bf612d09ea7e932`，Parent 为起点。
- 最终 I：`60e02674c5ef6bb3a3327f66b4e2761304a64af9`，Parent 为上述 I。独立 Debug 发现 GPUReady 夹具把 CreateEffect 错误回复当作 UploadProbe 读取；修正仅更正消费类型，保留 `kRuntimeIdentity` 及 compiled_graphs 断言。首轮 R/ASan 146/146、Debug 142/146 不是最终 I 的资格。
- V 的 Parent 为最终 I；V 仅新增本文件，其自身 SHA 在提交后的交付消息及 Git 中给出。
- 实施树：`E:/RenderV2Rebuild/IPCConvergence/source`；独立验证树：`E:/RenderV2Rebuild/C1C5/final-source`。`--no-hardlinks`、无 alternates、detached 精确 I、tracked snapshot 通过。
- 原始证据：`E:/RenderV2Rebuild/archives/60e02674c5ef6bb3a3327f66b4e2761304a64af9/C1-C5-evidence.zip`；SHA-256 `b489fbb702deb632f1b70126ec3d289532ec197a69a390a351fd31a5b072332a`，942份文件及逐文件索引，ZIP CRC复核通过。包括失败尝试、命令、日志、CSV、依赖版本、文件保护、逐文件 SHA-256、SDK 备份。首轮 clone 未完成即 checkout 的设置失败也保留；最终 clone 顺序完成并验证干净。

## 类型、命名与唯一所有权

表中 runtime/native 路径以 `modules/function/render/` 为根，platform 路径以 `modules/` 为根；完整逐文件、类型及方法/成员差异见归档中的 `final-source-inventory.json`、`final-source.patch`。

| 原类型/动作 | 本次实现及责任 | 源路径 |
|---|---|---|
| Impl.command_table / handlers | `CommandRegistry` 唯一拥有 CommandId 表及 HandlerId Owner 表；Impl 字段 `command_registry_` | runtime/pinclude/CommandRegistry.hpp、runtime/src/CommandRegistry.cpp |
| CommandHandler / CommandInvocation | `CommandBinding` 冷态描述；`HandlerCall{invoke,object}`；私有借用 `CommandDispatchContext` | runtime/include/.../CommandBinding.hpp、runtime/pinclude/CommandDispatchContext.hpp |
| Reply / RenderRequest / retryReplies | `ResponseOutbox` 唯一保存已就绪输出；Impl 字段 `response_outbox_`；`prepare/emplaceResponse/enqueue/flushTo` | runtime/pinclude/ResponseOutbox.hpp |
| DeferredCompletion / OperationCompletion | 唯一 `PendingOperation` 稳定槽拥有实际 Sender OperationState；原 CompletionQueue 交付事件 | runtime/pinclude/Deferred.hpp、runtime/src/CommandDispatch.cpp |
| CompileState / 特殊 adopt 分支 | 经用户裁决批准的私有 `CreateEffectSender::Operation`，候选只在 RenderThread 采用后成功 | runtime/src/ServerEffect.cpp |
| Texture/Mesh/Material/InstanceCommands | `*CommandHandler`；setTexture/updateTextureRegion/queryTexture/releaseTexture 等动作；Native 仍执行 Native 业务 | native/vulkan/include/.../{texture,mesh,material,instance}/*CommandHandler.hpp |
| IPC metadata / 原请求兼任回复资格 | `RequesterId{Name,epoch,Link}` 非拥有路由；QueueEntry 保存 tag/correlation；IpcMessage 只拥有 C++ 对象 | platform/ipc/include/.../IpcCore.hpp、IpcMessage.hpp、src/Ipc.cpp |

删除 `AsyncCommand.hpp`、`CommandHandler.hpp`、Runtime `Request.hpp/.cpp`、`ReplyAccess`、`CommandAsyncOps/Receiver/SenderOperation`、`ECommandOutcome`、RequestHeader/ReplyHeader；不留旧别名或转发头。`runtime/Commands.hpp` 保留名称，因为它确实是 Command 声明集合，不是注册表。

Server 动作更名为 `dispatchRequest`、`advanceRendering`、`detachExecutionGroup`、`findStorageSet`、`resizeTargetResources`、`attachEffect/detachEffect/readBackOutput`；`reserveCompletion` 由 `createPendingOperation` 取代。专业 Handler 不接触 IPC、Outbox 或 Reply。ResourceRegistry/DeviceSession/StorageSet 的 Owner 不变。

生产 C++ **+2289/-1671，净 +618 物理行**；测试 **+1106/-1099，净 +7**；CMake +17/-42、正式 README +131/-144 单独统计。改动生产文件中的具名 class/struct/enum 定义新增17、移除23（含重命名、嵌套定义，不计前置声明/别名/模板实例），净少6；新增/删除名单全部在证据中。不把物理行增加说成代码行减少，也不把历史文档删除算作成果。旧多层完成链已合并，现有测试按接口迁移并复用，未另建测试框架。

## 请求及生命周期追踪

同步：Client 构造 typed Request → `trySend(commandTag, requestId, message)` → Server 一次边界身份检查 → Registry 取两个指针 → thunk 在 Outbox 消息内直接构造错误初值 Response → Handler 原地填写 → Outbox 按 RequesterId+requestId 发布 → 原 Client 取得 typed Response。同步不分配 Pending；测例证实 Handler 返回前 Response 没有额外临时移动。

CreateEffect：先取得稳定 Pending 落点 → 成员返回具体 Sender → 唯一 Pending 拥有 connect 后的 Operation → CpuSchedule 执行原 `compileEffect()`，仅借稳定 FeatureEntry/Device → CompletionPost 投递既有队列 → RenderThread 检查终态并采用候选生成 EffectId → 通用 Receiver 投递最终事件 → Outbox 发布。Worker 不读写可变注册表，无 CompileState 并存。

GPU 上传：专业 Sender 保留移动后的 RenderPayload；其源 Owner 晚于 GpuWork/Transfer 结束。真实 Ticket、必要 Acquire、资源发布完成后才能产生成功；仍使用既有 Native 回调和同一完成队列。FAILED/CANCELLED 进入 Response 的 RenderError；已接纳 GPU 工作且副作用不确定时为 `kOutcomeUnknown`，关闭/abandon 不冒充 Native 取消。C1–C5 的新 IPC 不再隐式返还原 Request；异步输入由真正使用它的 Sender 保活，不能仅为路由而保留原消息。

事件循环没有无条件 sleep：请求、CPU 完成、stop 在现有 IpcCore 上递增同一 wake epoch 并通知；epoch 在检查工作前取得，阻止检查/入睡间丢唤醒。无进展时请求最多1ms的可唤醒 deadline，用于 GPU 完成观察及有限 Link 轮询；这是 OS 调度下的请求截止时间，不是硬实时保证。没有第二线程/调度器/队列。每次成功请求新增一次 release fetch_add 和 wake；CPU 通知、stop 复用它，回复端不新增原子计数。

停服先停止接纳、请求已有工作停止，清偿真实 CPU/GPU 工作与完成发布尾栈，交付/释放已就绪 Outbox，之后 reset Responder，再销毁资源/Handler/Device/Instance。Host 保持原模块代码加载至对象及回调尾栈结束；本轮不实现动态插件热卸载。

## 独立验证

| 配置 / 检查 | 最终 I 的实际结果 |
|---|---|
| RelWithDebInfo | 146/146；all 两轮，第二轮 no-work |
| Debug | 145/146；历史 `graph.edit` 失败，未提高 timeout/跳过/改判；两轮构建通过 |
| MSVC ASan | 146/146；all 两轮，第二轮 no-work；ASan Error=0 |
| Vulkan / SyncValidation | Error=0；已有 EOS Overlay 重复 Layer warning 保留，部分多 Session 用例累计多条 |
| 安装 / 公共头 / 外部扩展 | 矩阵内通过；另有全新安装前缀的 IPC 稳态消费者通过 |
| 本轮 API/职责、IPC 并发、专业 GPU 正确性 | Windows 已测范围 PASS |
| 性能 / 完整产品闭包 | PARTIAL / BLOCKED，不能概括成整体 PASS |

`object.target_contract` 本次 Debug 通过，但不宣称历史间歇失败已经修复。三项 GPUReady 的 Debug 类型断言失败在实施树修复，重新 clean clone 全矩阵验收，V 环境没有修复源码。CreateEffect 正常、调度失败、编译失败、停止、同步重入、重复请求/双 Worker、RenderThread 采用及回调尾栈已覆盖。

配置：Windows/MSVC 19.44.35226，C++20 `/GR-`；RTX 4070 Laptop GPU，Vulkan API 4211039、driver 2588573696；Validation + SyncValidation。lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`、toolset `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`。

命令与参数均在归档 `run-material.ps1` / `final-matrix.ps1` 及日志中。每配置执行：

```text
cmake -DLUX_SOURCE_DIR=<exact-I-clone> -P <clone>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <clone>/cmake/render-v2-bootstrap -B <fresh-build> -G Ninja -DBUILD_TESTING=ON ...
cmake --build <fresh-build> --target all -j 4 -- -k 0
cmake --build <fresh-build> --target all -j 4 -- -k 0
ctest --test-dir <fresh-build> -V --output-on-failure -j 1 -R .
```

ASan `/fsanitize=address /Zi`、`ASAN_OPTIONS=alloc_dealloc_mismatch=1`。构建与运行串行；独立树没有补源或修改测试。公开头、纯 IPC、纯 Client（禁 Vulkan/stdexec）、选定 Vulkan Host、外部 TestBuffer、移动前缀消费者均在矩阵中。GPU 正确性包括两 Client/多 View、3D color/depth、Texture/Mesh/Material/Instance、Readback、三 Queue 拓扑、LastUse T1/T2、部分提交及在途关闭。

## 成本与资格

实测 IpcMessage=96B/align16（原128B），实际 Request/Response QueueEntry=112B（原128B），ReceivedRequest=144B，HandlerCall=16B，ResponseRoute=32B，PendingOperation=272B，Native Binding=32B、RetirementEntry=104B。SBO 仍80B；8/32/64B 对象内联，128B 外置，未再次改变容量。布局来自 MSVC 实际类布局及测试输出。

下表 p50/p95/p99 为10个独立进程各自分位数的中位数，Max 为这10个进程的最坏单样本；完整原始值均保留。小 Command 每进程1000个、预热20个，完整构造/提交/处理/消费，EXE new=0、IPC DLL allocations=0。

| 新小 Command 分段，µs | p50 | p95 | p99 | Max |
|---|---:|---:|---:|---:|
| Submit | 0.10 | 0.20 | 0.95 | 8.90 |
| Handler | 0 | 0.10 | 0.10 | 3.70 |
| Outbox 发布 | 0.10 | 0.20 | 0.20 | 2.60 |
| 成功 Receive + 类型化访问 | 0.10 | 0.10 | 0.20 | 8.70 |
| 完整往返 | 0.70 | 0.80 | 1.75 | 50.70 |

计时粒度约100ns，0不表示没有成本；跨线程分段可重叠，不能直接相加。该夹具是持续往返，不代表空闲唤醒/GPU/宿主 sleep 延迟。独立预热 IPC 消费者（双线程未绑核、仅1个在途、4096次预热，不含线程启动）10k/100k/1M 的每-run总时长中位数为3.024/31.053/281.803ms，约3.31/3.22/3.55百万次往返每秒。含增长/线程启动的 FIFO 原始结果另存，未混称稳态。

| 同机交替对照 | 旧 p95 | 新 p95 | 限定 |
|---|---:|---:|---|
| 8B 双物理核 IPC | 0.40µs | 0.40µs | 最坏168.0→264.9µs，长尾未关闭 |
| 128B 双物理核 IPC | 0.60µs | 0.70µs | 新 p95 较高，未归因；最坏436.3→192.1µs |
| Native record/execute，1000次/run | 167.75µs | 164.20µs | 旧/新 p99 216.45/202.85µs；最坏1604.1/1773.8µs；不据此宣称无回归 |
| Alias 四 View 间隔 | 3211.15µs | 1540.65µs | 每窗口上传旧33–36次、新56–68次，工作量不等 |
| CPU normal，128操作，post→pop | 156.80µs | 101.85µs | 是实际完成事件测试，完整样本保留 |

GPU Ready 每拓扑1000次：Fence被CPU观察→Outbox发布的 p95 为36.3/37.9/38.8µs（alias/dedicated/same-family），但 Submit→Fence观察 p95 仍约15.8–16.5ms，发布→Client消费 p95 约14.8–15.0ms；包含宿主调度/等待，未把它称为 GPU 执行时间或纯 IPC 成本。旧报告的2.04ms Wait、14.49ms发布延迟属于历史口径，不直接当作本轮等价收益。

四 View 稳定帧 first-party new=0；相同 Shader 的相邻合法 Graphics Pass 仍为1 Pipeline/1 Bind/1 Scope/2 Draw，不同 Shader 为2/2/1/2。Main暂停消费1.5秒，三拓扑 Upload完成720–741次；GPU buffers创建/销毁相等。空闲200ms：134轮，GetThreadTimes报告0ns（低于该API可观察的CPU计时粒度，不宣称实际零CPU）。未运行 VTune，不对保留的尾样本作猜测性归因。

旧对照二进制来自独立 `2bd86e7eb026c44e93ca70043cbcf624dd72bbd2` 构建，其相关生产代码与 f6a 起点一致（差异仅文档/检查器清理、样例路径及测试 CMake，差异表归档）。新二进制为最终 I 的独立 Release。相同机器/工具链10对交替独立进程；保存每 run 原始样本及 p50/p95/p99/max，摘要取 per-run 分位数中位数，不拼接掩盖尾部。

Alias 使用相同1500ms场景窗口，但上传/提交次数会随事件循环进展改变，因此不是固定总工作量等价证明。保留 Native/Alias/IPC 及旧长尾的 **PARTIAL**；没有声称更快或无回归。小 Command 的1000次新路径分段没有等价旧夹具，单独报告。GPU 完成观察时间是 CPU 观察，不是 GPU timestamp（**NOT_MEASURED**）。Linux/Android 构建及 TSan **NOT_RUN**。

Legacy 719个工作树 Git blob 与 manifest 一致；依赖两仓干净且 SHA 未变。主工作树 `.vscode/settings.json`、用户 `Graph.hpp`，以及另一 V12 工作树用户 `RenderServer.hpp` 的原 SHA 全部一致；修改白名单通过。未改历史报告/判定。

精确 I 的26个公共头变更已同步 Debug、RelWithDebInfo、Android/lux-engine 三前缀：78个动作中包括18个旧头删除，全部逐文件备份、前后SHA校验。只同步头，不宣称这些全局前缀的旧库已完成产品重建；正式安装资格来自独立新/移动 SDK。Android头同步不代表Android构建。

Root TOOLCHAIN真实 configure exit=1：仍引用缺失的 `render/client`、`render/features/cmake/feature_client.cmake`、`render/standard_content`，未造占位 Target；根产品构建因配置阻断未执行。本轮无剩余新增正确性失败，历史 Debug/Root 与性能未归因项继续单列。

完成后只安全快进推送 `codex/render-v2-rebuild`，不合并、不强推。停止于 C1–C5；不进入 Graph/MaterialGraph、WSI/HDR 或下一轮渲染扩展。
