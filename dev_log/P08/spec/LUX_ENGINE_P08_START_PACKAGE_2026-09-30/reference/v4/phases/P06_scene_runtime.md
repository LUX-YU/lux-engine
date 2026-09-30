# P06 — 统一场景驱动、实例退休与独立 Run 会话

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P01, P02。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M08；引擎 SceneRuntime 受影响边界。**关联问题：** A02、T04、T05、T06。**V3 回归：** Q22, Q23, Q24, Q25, Q29, Q43, Q47。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `SceneInstanceLease / InstanceRetirement` | `engine/scene/composition/include/lux/engine/scene/SceneInstanceLease.hpp` | move-only 退休责任；host 存实际记录 | 不增加第二套 Runtime；析构不在 BUSY 回调中同步 destroy。 |
| `SceneRuntime 明确控制 API` | `engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp` | 原宿主原算法重整 | borrowInstance/borrowClock/pauseSimulation/resumeSimulation/requestStep/retireInstance/driveFrame。 |
| `RunId / RunInfo / RunProvenance / StepTicket / StopTicket` | `editor/tools/scene/execution/api/include/lux/engine/editor/scene/RunTypes.hpp` | 运行域值，不属于 Session 身份 | 票据指向真实一次步进/停止，区分 accepted/completed。 |
| `RunSession / RunStore` | `editor/tools/scene/execution/include/lux/engine/editor/scene/RunStore.hpp` | RunStore 独占 RunSession；不继承 SceneSession | 持启动快照、provenance、instance lease、执行状态；不持 UI 或作者可写引用。 |
| `RunController / StartRunOperation` | `editor/tools/scene/execution/include/lux/engine/editor/scene/RunController.hpp` | 具体启动/控制服务 | capture→prepare→owner 实例创建→发布 RunId；只借用既有执行器。 |
| `RunInspectAccess / RunningObjectRef` | `editor/tools/scene/execution/api/include/lux/engine/editor/scene/RunInspectAccess.hpp` | 轻量读取能力 | RunId+实例/对象代际隔离；UI 不取得 tick/EngineContext。 |
| `RunDebugEdits（仅保留已有临时调试能力）` | `editor/tools/scene/execution/src/RunDebugEdits.hpp` | 运行自己的编辑/undo 记录 | 暂停调试不改变作者源；stop 不回写。RunChangeProposal 未实现时明确不可用。 |

## C. 引擎侧与 Editor 侧分开修改

SceneRuntime 继续是实际场景记录与统一调度的唯一宿主。RunStore 管理“一次运行”的业务身份，不是另一个 Registry/worker/tick 宿主。SceneInstanceLease 表达某个使用者对该实例的退休责任；不能同时让 lease 和一段 scope_exit 都 destroy 同一实例。

### C1. 引擎 API 迁移表

| 旧 API/机制 | 新 API/动作 | 本阶段要求 |
| --- | --- | --- |
| `valid(SceneInstanceId)` | `resumeSimulation(...)` | 所有引用点实际迁移；方法名不再暗示存在性 |
| `invalid(SceneInstanceId)` | `pauseSimulation(...)` | 停仿真时间，不停维护/发布 |
| `getSceneRegistry(...)` | 受限 `borrowInstance(...)` 或确切 registry 访问 | 期限止于 driveFrame/结构修改/owner wait；不能保存裸引用到下帧 |
| `getClock(...)` | `borrowClock(...)` | 清楚借用而非复制时钟；保留原线程错误 |
| `tick()` | `driveFrame()` | 全产品同实例只有一个调用所有者；不与 UI update 再重复驱动 |
| `destroy(...)` 普通用户立即删除 | `retireInstance(...)` + 退休完成值 | 真正立即回收细节私有；资源/任务退休后才移除记录 |
| build 返回普通可遗忘 ID | build/createInstance 返回持退休责任的 lease 或将 lease 交给唯一 owner | ID 仍用于寻址；不是另一个 owner |

修改 EngineContext/Player/预览/测试/旧 editor 的真实调用方。限定名的旧 valid/invalid API 本阶段删除；不要留下 deprecated 同义名给下一位 LLM 继续使用。通用 `Id::valid()` 及其他不相关 tick/destroy 不能按字符串全局删除。

lease 析构不得进行分配、阻塞等 worker 或抛异常；退休记录在创建时/安全阶段预留。若不能保证此性质，就要求显式 retire 并在 owner 的受控容器析构时完成，不能用 destructor 中 terminate 掩盖正常 BUSY。Runtime 必须长于所有 lease 与退休记录；应用销毁顺序在 P12落实。

## D. Run 状态与票据

RunProvenance 保存 ContentStamp 和 RunConfiguration；实际启动使用不可变 SceneSnapshot。准备阶段失败无可见 RunId 或产生准确的失败 StartRunOutcome，绝不向 UI 公布一个尚未成功构建的运行实例。

运行状态是 Running/Paused/Stopping/Stopped/Failed 等互斥值，持相应资源；单步票据包含 RunId 与目标实际 simulation_completed 序号。连续单步请求的排队/拒绝策略固定为有界 FIFO，一次票据只对应一步；忙时容量满准确拒绝。不能使用一个 single_step bool 合并掉用户多次步进。

暂停时仍执行维护、完成采用、空间同步、渲染发布；恢复明确校准墙钟基准，不能补跑暂停期间巨大 dt。StopTicket 只有资源和该运行的任务按协议终结后才完成，不因 requestStop 入队立即成功。

## E. 作者隔离与旧字段去向

旧 SceneEditor::Impl 的 `run_scene/run_source/run_assets/run_receipt/run_status/run_delta/run_stop/run_prepared_/run_preparation/next_run/step_baseline_/single_step/run_connections/run_selection/run_catalog_changed/run_history/run_editing` 分别迁入 RunSession、StartRunOperation、RunInspectAccess/调试状态，完整去向见成员账本。新的 Run 不从旧窗口借用 source 或 history。

`play/pauseRun/resumeRun/stepRun/stopRun/runStatus` 的新调用面是 RunController/RunSession；UI 在 P10 仅调用受限控制/查询。旧 editor 的运行调用壳可以到 P12 前继续存在，但从本阶段起 runtime API 只用清晰的新接口，不保留双 API。

当前确有运行调试历史时保留为运行域独有的临时能力。停止不隐式回写。`ApplyRunChanges` 不作为本轮新功能强制引入：未实现时明确不可用并验证 stop 不回写；已经存在且产品依赖时必须迁成带基线冲突检测的一次作者 EditBatch，不能删除功能冒充未实现。

## F. 阶段出口

分别测试实际运行宿主和 fake 调度边界。关闭/资源背压涉及 GPU 的最终验证在 P10/P13；本阶段至少运行实际无渲染/可用配置的 scene drive 测试，不能只测空 FakeRun。PLAYER profile 在无 editor 配置下仍可构建与运行相应测试。为同实例一帧 drive 次数加测试计数，计数设施只进测试支撑，不暴露可写产品后门。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| SceneRuntime::valid/invalid | 修改所有限定调用并删除旧声明、定义和兼容 alias | pauseSimulation/resumeSimulation；P06 | 其他身份类型 valid() 不动 |
| SceneRuntime::tick/getSceneRegistry/getClock/destroy 的旧公开调用形状 | 迁到明确驱动/借用/退休 API；立即删除能力收回宿主 | driveFrame/borrowInstance/borrowClock/retireInstance；P06 | 实际维护/同步/发布与错误检测机制保留 |
| SceneEditor::Impl::scenes_guard_/destroyScene 与相同实例的重复释放责任 | 旧消费也改成单一 lease 或单一显式退休责任，不双删 | SceneInstanceLease / host retirement；P06 | 已验证算法、资产兼容读取与行为回归不删 |
| 旧 Pane 内 run_*、single_step/step_baseline_、play/stop 等 owner 实现 | 纯运行算法归新 execution；旧入口限期暂留 | RunStore/RunSession/RunController；P12 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X06-01 / `one_runtime_driver_per_instance` | 同 frame 同时有作者投影、运行与两个视图消费者 | 实例实际仿真/维护推进不重复，无 editor 私建 Runtime |
| X06-02 / `pause_maintains_without_time_jump` | 暂停时注入资源完成，再恢复 | 维护前进，仿真时间停住，恢复不补巨大 dt |
| X06-03 / `step_tickets_track_actual_steps` | 连续 step、执行失败、旧 RunId 与队列满 | 每张成功票据对应真实一步；错误不假完成 |
| X06-04 / `retire_in_callback_is_deferred` | 回调中请求停止/退休，存在飞行资源 | 不立即 destroy 当前对象、不阻塞/terminate；安全点最终释放一次 |
| X06-05 / `run_snapshot_is_not_author_source` | S10 启动后作者改到 S12，运行又改对象 | 两份语义隔离，stop 不回写，provenance 为 S10 |
| X06-06 / `player_build_without_editor` | PLAYER 配置完全不包含 editor targets | 引擎运行场景仍可用，未产生反向 Editor 依赖 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P06.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
