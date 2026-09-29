# P05 R2 复审结论与 P06 启动指令

**日期：2026-09-29**  
**项目：LUX-YU/lux-engine**  
**实施分支：`codex/editor-redesign-v4`**  
**复审验收 HEAD：`fae890a2a848ee68a16ff976e8047ea1b2a3a560`**  
**P05 R2 实现：`3a75bfe6f743d8b9bb74f93f89219054aee2119d`**  
**R2 前置验收：`5a1ccbb47d2d08ffa8cfda907f74319c4a694068`**

## 0. 结论、证据范围及文件地位

**P05 R2 在本次复审范围内通过，允许进入 P06；只授权 P06，完成后停下复审，不自动进入 P07，不修改 main。**

本次读取了远端 HEAD、提交父链、实现差异、SaveService 的完成吸收及调度保护、真实执行器测试、R2 验收说明与验证器、部分修复前后日志和完整 CTest 的汇总部分。没有发现必须继续阻止 P06 的前置缺口。[S1–S9]

另外独立运行了两份与 Git Blob 一致的生产核心 `.cpp`，使用上一轮隔离测试的基础头替身。GCC 14.2、O0/O2、ASan/UBSan 下，六种场景共十二次运行均退出 0：编码成功、编码错误、自撤销、递归采用、保留 W1 对照、确认 W1 后版本衔接。

**独立运行不是完整 Lux SDK 验收。** `expected`、身份及 CodeLease 等基础头使用 shim；完成由测试手动交付；没有编译真实 SaveExecution/ExecutionRuntime/stdexec，也没有运行三模型、真实文件 IO、GPU 或 Android。C++23 仅用于隔离程序的 `std::expected`，不要求项目升级 C++20。独立探针关闭 leak detector，不宣称已经证明无泄漏。结果、命令与源身份放在启动包的 `review/probes/`。

仓库的 **116/116 CTest、九组 33/33 安装消费者** 是实施方归档成绩。源码和测试阅读、核心隔离复跑不等于我独立执行了那套完整矩阵，也没有逐份重算全部远端归档哈希。

本文是原 V4 `phases/P06_scene_runtime.md` 的启动补充，不另创架构。第 3 节以后均为**下一阶段实施要求**，不是声称已有新 Runtime/Run 类型。原文档及追踪索引在启动包 `reference/v4/` 中保持不变；不得用其历史种子账本、旧路径和脚本覆盖当前仓库。

## 1. P05 R2 为什么可以结束补正

### 1.1 完成事实与业务重入已经分开

`completeEncoding()` 不再因为当前 dispatching 而拒绝合法的 ENCODING 完成，但仍检查 owner、操作 ID 及阶段。它只结算既有操作的额度和原写票据，不调用保存角色、不采用基线、不销毁 rebind、不删除 operation。

完成路径不再调用可能清理 job 的 `releaseSnapshot()`，而只将已经转交给 EncodeWork 的快照计量归零并扣减。任务与代码寿命继续由已经持有它们的任务对象负责。这使“叶子完成吸收”的职责与实现一致。[S3]

`DispatchScope` 保存进入前的值，析构恢复该值。内层完成返回后，外层 describe/capture/accept 的保护仍为 true；不重新开放递归采用、注册、新保存、取消或确认删除。

### 1.2 SaveExecution 不再吞掉合法完成交付

成功、域错误、取消和 task admission 失败统一进入私有 `receiveEncoding()`，检查服务是否接受完成。正常 owner 上的合法一次完成可以接收，无需结果队列、重试或重新运行 encoder。

这里的 `terminate` 只用于适配器破坏恰好一次／owner／有效操作前提的契约错误，不是把正常 BUSY 变成终止。服务直接调用者仍得到 UNKNOWN_ID、WRONG_THREAD、非 ENCODING 的结构化错误。以后不得重新引入一种适配层正常可遇到但只能被终止处理的暂时拒绝。[S2–S3]

### 1.3 测试检查了真正的集成点

| 场景 | 已读实现与归档所覆盖的内容 |
|---|---|
| accept 中首次 collect | 真实 TaskInfo.finished 成立后才收集，非固定 sleep；真实 SaveExecution 和 Material 编码；检查 encoding_calls=1、stuck_encoding=0 |
| 错误、取消与停止 | 实际 MaterialCodec 身份不匹配错误；Runtime stop 与保存取消；NotPublished 保留准确错误；同目标后继继续 |
| 外层保护 | 完成已吸收后，仍在 accept 栈内尝试 adopt/ack/request/cancel，确认未解除保护、未重复调用角色 |
| describe/capture | 正常返回、普通异常、注册撤销，各重复十二次；其他操作已完成，外层失败只回收自己的额度 |
| 容量与寿命 | 六组合共七十二次；两个快照重新可用、第三个准确拒绝；一次编码/析构、结束后无残留票据；真实 task_capacity 拒绝可结清 |
| 继承回归 | R1 角色及版本衔接测试、原模型和实际 IO 继续登记；原断言保留检查由阶段验证器执行 |

修复前真实 SDK 日志是 `collected=1 encoding_calls=1 stuck_encoding=1`，取消后仍 ENCODING/RESERVED，W3 READY 被阻塞，文件停在 material1。修复后关键日志是 `collected=1 encoding_calls=1 stuck_encoding=0`。原失败仍保存。[S4–S8]

### 1.4 不再增加 P05 修复任务

没有新增队列、线程池、target、公共框架或过渡桥。生产范围是 SaveService、SaveExecution 和对应契约，不重做 WriteCoordinator、三模型、History 或 SessionState。原 R1 版本衔接方案保持。

本次通过只说明已审阅的 P05/R1/R2 契约可以作为 P06 前置；不代表整个 Editor 已完成迁移或获得最终产品资格。

## 2. 交接时必须保留的工程事实

### 2.1 固定提交与工作区

远端本次验收 HEAD 的直接父提交就是 R2 实现。后续从实际分支继续，不能每阶段重置到原 `2bb33ff1…`，也不能强推覆盖后来工作。

R2 README 说明：主工作区仍有既有 `editor/project/src/ProjectBuilder.cpp` 用户修改，资格使用独立干净检出的实现 SHA，未包含该改动。下一实施者先核对并保护；不要以“验收检出干净”代替“用户工作区没有修改”。我没有访问用户本地工作区。[S8]

### 2.2 冷构建顺序风险不是已经修复

R2 明确复用了既有独立构建目录，不宣称本轮首次冷构建成功。Physics2DDescription 在静态类型头生成前开始编译的风险仍未修复，记录的责任入口为：

`engine/domain/simulation/builtin_systems/physics2d/CMakeLists.txt`

历史失败证据保留于：

`dev_log/P05-R1/logs/development/f2b5a00c60e93dc81d7c72b127ad08b219ccfa23/build.log`

该项不要求在进入 P06 前另做大重构，也不归入 C01/C03/C04。若阻塞 P06 的实际配置或构建，应明确登记并作最小生成依赖修复，单独说明修改与验证；不能靠多跑一遍后删除第一次失败。最终 clean-build 资格必须处理或明确不通过。[S8]

### 2.3 证据保持不可变

继续使用 `.internal/editor-redesign/` 为唯一可变施工材料，阶段末冻结到 `dev_log/P06/`。原阶段的源码核验按其 implementation_sha，禁止为了适配新 API/目录改写旧收据。

C01（P09/P12）、C03（P11）、C04（P12）保持原 FAIL 及原责任。P06 新问题必须独立登记，不挂到旧编号延期。

## 3. P06 的目标：运行独立，但不新增一套 Runtime

原 P06 要求：**统一场景驱动、实例退休与独立 Run 会话**。[P06]

P01～P04 建立了作者源与历史，P05 建立了可靠持久化。P06 不是把作者 Session 加上 play/pause 成为万能对象，而是让一次运行有自己的来源、状态与结束责任。

```text
应用装配根
  ├── 原有 ExecutionRuntime
  ├── 唯一 SceneRuntime（实际实例、场景调度、退休记录）
  ├── SessionStore → SceneSession（作者源 + 原 History + SessionState）
  └── RunStore → RunSession
                    ├── RunProvenance + 启动快照
                    ├── SceneInstanceLease（一次退休责任）
                    └── 运行状态、步进/停止结果与必要运行调试状态
```

SceneRuntime 实际拥有实例记录；lease 表达使用者对该实例的退休责任，不成为第二个实际实例容器。RunStore 管理运行的业务身份，不成为另一套 Registry/worker/frame driver。

作者 S10 启动后继续编辑到 S12，RunProvenance 仍指向 S10；运行修改不写入作者内容，stop 不隐式回写。未实现的 ApplyRunChanges 不强制新增；原产品已经依赖的能力不得被删除后称作“不支持”。

## 4. 类型、关系与源码归属

以下目标名称来自原 P06。等价既有机制优先复用；没有行为实现与测试的空接口不算交付。

| 类型/角色 | 归属 | 关系与唯一责任 |
|---|---|---|
| SceneInstanceLease、InstanceRetirement | `engine/scene/composition/`；原计划公开头 SceneInstanceLease.hpp | move-only 退休责任；Runtime 持实例记录。不可复制，不同时配一个 destroy scope_exit |
| SceneRuntime 的明确控制 API | 同一引擎模块 | 继续使用原宿主和驱动算法；分清维护、仿真、同步、发布与退休 |
| RunId、RunInfo、RunProvenance、StepTicket、StopTicket | `editor/tools/scene/execution/` 内的轻量 API 头 | 运行身份不同于 SessionId；票据区分获准与实际完成 |
| RunSession、RunStore | `editor/tools/scene/execution/` | Store 独占 Session；RunSession 不继承 SceneSession，不持可写作者源或 UI |
| RunController、StartRunOperation | 同一 execution 主题 | 组合启动捕获/准备/实例创建/发布；只借用已有执行器、宿主和受限作者访问 |
| RunInspectAccess、RunningObjectRef | 同主题轻量 API | 使用 RunId/实例/对象身份验证，不向 UI 暴露驱动与完整 EngineContext |
| RunDebugEdits | 同主题私有实现 | 只保留原已有运行临时调试能力；不复制通用 History 算法，不污染作者撤销栈 |

目录继续保持主题聚合：共用历史与会话位于 `editor/editing/history/` 和 `editor/editing/sessions/`；运行相关都在 `editor/tools/scene/execution/` 内。原 `execution/api` 可作为轻量头边界，但不是要求每个票据、控制器、helper 单独建立库/DLL。不得重建根目录 `editor/history`、`editor/sessions`。

## 5. 当前引擎接口与明确迁移要求

本次另读取了当前 `SceneRuntime.hpp`：旧 valid/invalid/getSceneRegistry/getClock/destroy/tick 仍在，Builder::build 返回 SceneInstanceId。下表的新名字是目标接口，不是已经完成的实现。[S10]

| 当前限定接口/责任 | 目标语义 | P06 退出要求 |
|---|---|---|
| SceneRuntime::valid(SceneInstanceId) | resumeSimulation | 删除原声明、定义、调用和同义别名，不再借“有效性”命名表达推进开关 |
| SceneRuntime::invalid(SceneInstanceId) | pauseSimulation | 暂停仿真时间，维护与发布仍有准确行为 |
| getSceneRegistry 的 const/non-const 重载 | borrowInstance 或明确受限 Registry 借用 | 说明借用结束点；不能在 owner wait/结构改变之后沿用裸引用 |
| getClock | borrowClock | 明确借用、线程与阶段约束，不误称独立时钟副本 |
| tick、TickResult 及驱动 owner | driveFrame 及语义一致的结果 | 核对整个调用面，一帧一个真实驱动 owner；不保留另一条旧 public 驱动入口 |
| public destroy | retireInstance + 可观察退休完成 | 立即移除细节私有；正常 BUSY/回调内停止不能通过 terminate 处理 |
| Builder::build 返回普通 ID 后的释放责任 | 返回/移交 lease，或原规范允许的单一显式责任 | 寻址仍用 ID；每实例只有一个负责请求退休的 owner |
| scenes_guard_/destroyScene/其他相同实例释放器 | 单一退休责任 | 移除重复 destroy；移动和失败回滚也不得重复请求最终释放 |

精确限定名清单与调用方以当前 AST/引用盘点为准。必须同步 EngineContext、PLAYER、旧编辑器、预览、运行测试和安装消费者。不可全局文本删除其他类型的 `valid/tick/destroy`；不可删除仍然提供独立用途的普通 Id::valid。

本阶段迁走的旧算法体与双 API 必须退出活动源码、生成输入和导出。旧窗口的 UI 控制壳按 P12 暂留；不得因此继续维护两份纯运行算法或两份实例释放责任。

## 6. P06 内部实施顺序（仍是一个阶段）

### P06-A：先固定责任与调用图

核对所有 SceneRuntime 调用方、实际 frame owner、现有 Clock 与 SceneDriveSnapshot 指标、run_* 成员、停止/异常清理路径。每一实例写清：谁创建、谁借用、谁驱动、谁请求退休、谁在何条件下最终释放。

将原成员账本的旧 SceneEditor run_scene/run_source/run_assets/run_receipt/run_status/run_delta/run_stop/run_prepared_/run_preparation/next_run/step_baseline_/single_step/run_connections/run_selection/run_catalog_changed/run_history/run_editing 对照实际代码，逐项确定新 owner、暂留消费者和期限，不按旧表盲搬。

### P06-B：先实现引擎侧退休与借用契约

在原 SceneRuntime 内完成明确控制、借用和实例退休。退休记录在创建或安全阶段预留；析构请求退休不得分配、阻塞等 worker 或在正常 BUSY 时抛错/terminate。

回调内停止只登记退休意图，之后由安全点推进。运行中的任务与飞行资源尚未安全退出时，不提前删实例记录。请求成功不等于回收完成。若选择显式 retire 而非自动 lease 析构，受控 owner 必须可靠履行责任，不把 API 失败丢弃。

### P06-C：暂停、恢复与单步的真实控制

暂停不推进 simulation time，但继续执行必要维护、完成接收、空间同步与渲染发布。恢复重新校准墙钟，不能将暂停间隔作为一次巨大 dt 补给仿真。

单步使用有界 FIFO。每个成功票据对应一次真实 simulation_completed 的推进；重复点击不能被一枚 bool 合并。区分排队容量拒绝、实际执行失败、旧 RunId 与已经完成的请求。

### P06-D：独立启动与 RunStore

通过已验收 SceneSnapshot 捕获启动来源，准备失败不发布半成品 RunId；实际构建成功后由 RunStore 拥有 RunSession，关联原 SceneRuntime 中的实例及其 lease。

后台准备只拥有输入；需要 owner 亲和性的实例、状态与历史在 owner 构造。不得用旧 SceneEditor::Impl 包装新 RunSession，也不得同步维护第二份作者源。停止后保留准确终态和必要查询，不无界保留已退休资源。

### P06-E：迁移调用方并删除到期接口

旧产品尚未完成 P12 切换，但相关调用方也必须使用唯一新 Runtime API。必要旧接线只做输入/结果转换；如果旧窗口尚未由新 SceneSession 提供作者内容，可使用明确的冻结输入适配，不能为配合新 Run 而建立一份长期双向同步的 SceneSession 副本。

运行调试状态只属于本次运行。运行 UI 仍是观察者/命令来源，不获得实际驱动或立即销毁权。

### P06-F：验收与安装

运行下节六项原 X06、相关 Q 及原模型/持久化回归，实际引擎宿主与必要 fake 边界互补。PLAYER 不依赖 editor。同步公共头、包、真实消费者与阶段路径检查，冻结确切实现 SHA 的证据。

内部步骤可以分提交，但不新增六个阶段或要求每小步重新批准。范围扩大必须解释真实依赖，不能借机重写 Lua、渲染后端、执行器、资源格式或全局插件系统。

## 7. 必须继承 P05 R2 的交接原则

### 7.1 准入与完成不是同一种操作

Run 的新 start/step/stop 请求可以按容量或状态拒绝；**已经接受的准备/驱动/退休工作的完成必须可靠结算**。不能让回调保护把这类结果 BUSY 后丢弃，更不能通过重跑工作来掩盖。

只记录完成事实的内部操作与调用 UI/用户逻辑的阶段分开。若内层接收发生于外层保护中，必须恢复之前的保护状态；不得内层结束就解除外层的防重复、防删除约束。

### 7.2 不复制一组 busy 标志

SceneRuntime 管其驱动和退休阶段；RunStore/Controller 管运行业务记录；作者 SessionState 管作者编辑准入。三者不是同一件事。状态应各自拥有且通过明确操作关联，不能几处同时维护同一份 run_busy 或 current。

### 7.3 生命周期不靠字符串与回调顺序猜测

已冻结启动数据、代码/元信息、实例 lease、飞行任务及资源必须按真实使用关系释放。身份与代际验证仍须存在；代码保活不意味着对象仍注册，稳定地址不意味着执行期可以被删。

P06 不需要修改已经修正的 SaveService。只需让其完整回归继续通过，并把“完成可靠接收、采用在安全点、资源最后释放”用于本阶段自己的真实边界。

## 8. 原 X06 的验收映射与范围

| 原编号 | 必须验证 | 防止的错误完成声明 |
|---|---|---|
| X06-01 one_runtime_driver_per_instance | 同 frame 多消费者下，真实实例仿真/维护不重复驱动 | 只给新接口改名，旧 UI update 仍再次 tick |
| X06-02 pause_maintains_without_time_jump | 暂停中注入完成，维护继续；恢复无累计大 dt | 暂停把整个 Runtime 停死，或只测时钟枚举 |
| X06-03 step_tickets_track_actual_steps | 连续 step、失败、旧 RunId、队列满；一次票据对应真实一步 | single_step bool 合并多次请求；获准立即当完成 |
| X06-04 retire_in_callback_is_deferred | 回调内请求退休、有在途资源；安全点最终只释放一次 | 回调立即销毁、同步等待或正常 BUSY 下终止 |
| X06-05 run_snapshot_is_not_author_source | S10 启动，作者 S12，运行修改；stop 无回写，来源仍 S10 | 借 live 作者源、回写作者 History、stop 重置原 Session |
| X06-06 player_build_without_editor | PLAYER 无 editor targets 时仍构建并测试运行宿主 | 引擎控制为复用 Editor 类型而反向依赖 editor |

原 P06 第 F 节允许本阶段至少使用实际无渲染/可用配置的 scene drive 验证。不要为 X06-01 提前实现 P07/P10 的完整投影与双窗口产品：可在真实宿主上用有界的轻量消费端验证驱动唯一性，保留已有桌面/GPU 回归，准确标明真实双视图产品验收仍属后续。也不能仅用空 FakeRun 替代实际宿主行为。[P06]

原 116 项测试、R05-R2-01～05、R05-01～08 与相应语义必须保留；旧 Runtime API 迁移所需测试修改可以发生，但要逐项解释新入口等价验证，不能将旧断言直接删掉。安装消费者按实际接口迁移后运行；不以测试总数替代每项行为。

最终显式 `LUX_EDITOR_MIGRATION_STAGE=P06`。依赖负例验证引擎不依赖 editor、新 Run 不依赖旧大 Impl/UI、作者 model 不反向依赖运行。实际 CMake 禁止边要命中正确规则，同一夹具修复后成功。

## 9. 交付收据与禁止续行

P06 收据至少记录：实现/验收 SHA、Runtime 与 RunStore 所有权、创建/借用/驱动/退休图、票据容量与失败语义、全部旧 API 删除项、旧窗口暂留项及消费者、六项 X06、相关 Q、原回归、实际运行/PLAYER/安装与依赖测试。

明确区分：请求获准、任务完成、实例停止推进、任务退休、资源可安全释放、实际回收完成。不得把它们统一写成一句“停止成功”。

实现与验收分别提交，正常推送，停在 P06。不更改原阶段快照，不 force-push，不修改 main，不自动进入 P07。必测缺失、环境阻塞或到期接口残留应为 PARTIAL/BLOCKED，而非 PASS。

## 10. 可直接交给实施方

> P05 R2 复审通过，允许进入 P06，仅执行 P06。以 fae890a2a848ee68a16ff976e8047ea1b2a3a560 为已验收前置，核对实际工作区与祖先关系，保护 ProjectBuilder.cpp 的既有修改，不 reset、不修改 main。
>
> 读取原 P06、本启动补充和当前施工账本；按 P06-A～F 实施原 SceneRuntime 的明确控制/借用/退休与独立 RunStore/RunSession。保持一个真实 Runtime、一个 frame 驱动 owner和每实例一个退休责任；Run 不继承 SceneSession、不借可写作者源。
>
> 移除 SceneRuntime 的旧 valid/invalid/tick/getSceneRegistry/getClock/destroy 调用形状及同义兼容入口；所有实际调用方同步迁移。旧窗口 UI 壳按期限暂留，纯运行算法只保留一份，禁止双释放。
>
> 暂停继续维护与完成接收，恢复无巨大 dt；单步采用有界 FIFO 与实际完成票据；退休请求与真正回收分开，回调内不立即销毁或阻塞。保留已有运行调试功能，停止不隐式回写作者源。
>
> 继承 P05 R2：已接受工作的完成不能因业务防重入而丢失，内层接收不解除外层保护；不新增通用管理器、结果队列或线程池来替代准确职责。
>
> 运行 X06-01～06、相关 Q、原 116 项与持久化/R1/R2 回归、实际 scene drive、PLAYER 无 editor 配置、依赖负例和安装消费者；最终显式 P06 门禁。冷构建顺序风险单独保留，不挂旧缺陷，不冒称已修复。
>
> 沿用收拢目录和 .internal 唯一施工材料，阶段末冻结 dev_log/P06。C01/C03/C04 保持原 FAIL/责任。实现与验收分别提交并正常推送，停在 P06 等待复审，不自动进入 P07。

## 11. 固定来源与历史规范

- [S1：本次验收提交及父链](https://github.com/LUX-YU/lux-engine/commit/fae890a2a848ee68a16ff976e8047ea1b2a3a560)
- [S2：R2 实现差异](https://github.com/LUX-YU/lux-engine/commit/3a75bfe6f743d8b9bb74f93f89219054aee2119d)
- [S3：当前 SaveService](https://github.com/LUX-YU/lux-engine/blob/fae890a2a848ee68a16ff976e8047ea1b2a3a560/editor/persistence/src/SaveService.cpp)
- [S4：真实执行器及模型测试](https://github.com/LUX-YU/lux-engine/blob/fae890a2a848ee68a16ff976e8047ea1b2a3a560/editor/tests/persistence/models.cpp)
- [S5：修复前真实成功编码路径失败记录](https://github.com/LUX-YU/lux-engine/blob/fae890a2a848ee68a16ff976e8047ea1b2a3a560/dev_log/P05-R2/before/success.log)
- [S6：修复后同路径记录](https://github.com/LUX-YU/lux-engine/blob/fae890a2a848ee68a16ff976e8047ea1b2a3a560/dev_log/P05-R2/logs/persistence-r2-accept-success.log)
- [S7：116 项 CTest 归档](https://github.com/LUX-YU/lux-engine/blob/fae890a2a848ee68a16ff976e8047ea1b2a3a560/dev_log/P05-R2/logs/ctest.log)
- [S8：R2 验收说明及保留限制](https://github.com/LUX-YU/lux-engine/blob/fae890a2a848ee68a16ff976e8047ea1b2a3a560/dev_log/P05-R2/README.md)
- [S9：归档检查器](https://github.com/LUX-YU/lux-engine/blob/fae890a2a848ee68a16ff976e8047ea1b2a3a560/dev_log/P05-R2/check_receipt.py)
- [S10：本次读取的 SceneRuntime 旧接口](https://github.com/LUX-YU/lux-engine/blob/fae890a2a848ee68a16ff976e8047ea1b2a3a560/engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp)
- P06：原 V4 `phases/P06_scene_runtime.md`，启动包 `reference/v4/` 保留原相对目录及配套索引；不改写历史设计。
