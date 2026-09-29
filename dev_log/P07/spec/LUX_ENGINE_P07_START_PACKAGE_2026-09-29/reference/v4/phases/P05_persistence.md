# P05 — 冻结保存、有序发布、Save As 与三类编码适配

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P01, P02, P03, P04。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M06 / M07；包括 FlowForge 持久化适配。**关联问题：** A08、T04、T07、T08、C02。**V3 回归：** Q11, Q12, Q13, Q14, Q15, Q16, Q17, Q18, Q50。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `SaveId / SaveRequest / SaveStatus / SaveOutcome` | `editor/persistence/include/lux/engine/editor/persistence/SaveTypes.hpp` | 业务值与互斥结果 | 区分 admission、运行状态、已提交事实、采用结果、取消及不确定发布。 |
| `FrozenSave / EncodedArtifact / IEncodeJob` | `editor/persistence/include/lux/engine/editor/persistence/EncodeJob.hpp` | 异步边界接口；FrozenSave 拥有输入 | 无 Session/Pane 裸指针；encoding 输出拥有字节，CodeLease 在外层 owner 中长于 job 虚析构。 |
| `ISaveSource / SaveSourceRegistration` | `editor/persistence/include/lux/engine/editor/persistence/SaveSource.hpp` | 具体会话的角色适配 | owner 上 captureForSave / accept；注册销毁先于 Session；不继承会话或 UI。 |
| `SaveService / SaveOperation` | `editor/persistence/include/lux/engine/editor/persistence/SaveService.hpp` | 服务拥有稳定操作记录 | requestSave/status/requestCancel/acknowledge；worker 仅投递 ID+拥有结果。 |
| `WriteTargetKey / WriteTarget / WriteTicket / WriteLane` | `editor/persistence/include/lux/engine/editor/persistence/WriteLane.hpp` | 同目标顺序机制 | 准入时订票；编码可并行，发布只调度队首；失败/取消/不确定分别处理。 |
| `WriteCoordinator` | `editor/persistence/include/lux/engine/editor/persistence/WriteCoordinator.hpp` | 应用拥有、所有写入生产者借用的唯一具体协调器 | reserve / provideEncoded / cancelBeforePublish / status / reconcile；内部拥有 WriteLane，SaveService、SaveAs、编译发布、WorkspaceStore 共用同一实例。 |
| `IArtifactStore / PublicationOutcome / CommitReceipt` | `editor/persistence/include/lux/engine/editor/persistence/ArtifactStore.hpp` | 真实 IO 副作用接口 | Published / NotPublished / PublicationUnknown；持久性保证独立字段，不伪称全局 CAS。 |
| `ProjectArtifactStore` | `editor/adapters/project_io/include/lux/engine/editor/io/ProjectArtifactStore.hpp` | final : IArtifactStore | 复用现有 ProjectStorage/文件系统；规范化同目标别名与实际版本检查。 |
| `SceneSaveSource / SceneEncodeJob / SceneCodec / ScenePersistenceAccess` | `editor/tools/scene/persistence/include/lux/engine/editor/scene/SceneSaveSource.hpp` | 具体适配，不拥有 SceneSession | capture 内容戳/绑定；显式编码输出；受限基线采用。 |
| `MaterialSaveSource / MaterialEncodeJob / MaterialCodec` | `editor/tools/material/persistence/include/lux/engine/editor/material/MaterialSaveSource.hpp` | 对应材质源的具体适配 | 源保存不是编译产物发布；节点快照与 lease 完整。 |
| `FlowSaveSource / FlowEncodeJob / FlowCodec` | `editor/tools/flowforge/persistence/include/lux/engine/editor/flowforge/FlowSaveSource.hpp` | 对应 Flow 作者源的具体适配 | 编码快照环境而非读取 live FlowGraph；不执行 linker。 |
| `SceneSaveAsOperation / EncodedSceneClone / PreparedSceneRebind` | `editor/tools/scene/persistence/include/lux/engine/editor/scene/SceneSaveAs.hpp` | 具体用例组合 permit/candidate/write ticket | 克隆 package 与映射显式输出；发布前备妥采用，成功不清历史。 |
| `MaterialSaveAsOperation / FlowSaveAsOperation / SaveAsOutcome` | `editor/tools/material/persistence/include/lux/engine/editor/material/MaterialSaveAs.hpp；editor/tools/flowforge/persistence/include/lux/engine/editor/flowforge/FlowSaveAs.hpp` | 具体用例；不建立万能基类 | 绑定冻结范围相同；各自处理格式身份；Export Copy 不改来源。 |
| `PreparedSceneData / PreparedMaterialData / PreparedFlowData` | `editor/tools/{scene,material,flowforge}/persistence/include/lux/engine/editor/{scene,material,flowforge}/PreparedData.hpp` | 拥有解码产物值 | 后台 decode 与 owner 构造分离；P11 通过 SessionLoadJob 接入。 |

## C. 本阶段是一条完整的持久化事实链

顺序必须是：建立 source registration → 请求保存并在规范化目标 lane 预留 ticket → owner 捕获已提交内容 → worker 编码 → 队首有序发布 → owner 采用回执 → 终态可观察。不能只抽取一个 EncodeJob，再继续由窗口维护 save_ 和完成回调。

`WriteCoordinator` 由应用组合根拥有，它是同物理目标有序发布的唯一 owner；SaveService、各 SaveAsOperation、编译产物发布和 WorkspaceStore 借用同一个实例。WriteLane 只能由该协调器拥有，不允许每个生产者各建一条能写同一文件的 lane。它不拥有 Session，不理解 dirty，也不成为通用异步任务系统。

`SaveService` 不知道 SceneSession/MaterialSession/FlowSession 的实现，通过 ISaveSource 访问角色。角色持有代际 key 与受限会话访问，不持有内容 shared_ptr，不调用 Pane。ISaveSource 的注册 token 必须 RAII 可撤销；撤销后迟到结果仍在 SaveService 记录真实磁盘事实，但不能调用已销毁适配器。

## D. API、内部状态和线程契约

| API | 立即返回什么 | 后续完成事实 |
| --- | --- | --- |
| `requestSave(SessionId)` | SaveId 或准入失败；明确拒绝未绑定目标/容量满/绑定切换中 | SaveOutcome；不在准入时宣布“已保存” |
| `status(SaveId)` | owning 小快照/结构化 UnknownId | 不返回可变 operation 或失效 span |
| `requestCancel(SaveId)` | CancelRequested / TooLate / AlreadyTerminal 等请求处置 | 终态由实际发布边界决定，不立即假装取消 |
| `acknowledge(SaveId)` | 只针对终态的观察清理结果 | 不取消 IO，不删除正在被 GPU/worker/插件使用的资源 |
| `ISaveSource::captureForSave()` | FrozenSave，包含 ContentStamp、BindingRevision、确定目标与拥有编码输入 | 无外部可写 `copied` 或调用者仍需保持 alive 的局部引用 |
| `ISaveSource::accept(SaveReceipt)` | Applied / StaleHistory / StaleBinding / Closed / OlderReceipt | 磁盘成功与会话是否适用分开保留 |

内部使用互斥阶段记录：Captured/Encoding、ReadyToPublish、Publishing、AwaitingAdoption、Terminal。相关资源放在阶段内，不再在旁边堆 `pending/done/has_result`。operation 的存储地址稳定或回调只拿 ID；不让 vector 扩容、variant 移动使捕获 this 悬空。

owner 负责捕获、注册变更、会话采用；worker 负责拥有输入的编码/IO。复用既有 ExecutionRuntime/stdexec，不能为保存额外创建线程池、常驻后台线程或通用 OperationManager。完成采用通过已存在完成分发设施进入 owner 阶段。

### D1. 写入顺序与失败空洞

W1 捕获 S10 先准入，W2 捕获 S12 后准入，即使 W2 先编码完成，也只能先发布 W1 再发布 W2。W1 编码失败/确认未发布取消后，明确终结票据，lane 才能前进。用户 undo 回 S8 后再 Save 是一个新的 ticket，不能因为 StateId.serial 较小而丢弃该保存意图。

同一物理目标的别名由 ProjectArtifactStore 规范化；明确大小写、相对路径、符号链接与项目资产映射的支持范围，不能只比较路径字符串。跨进程锁/CAS 未实现就只承诺本协调器内顺序与外部修改检测。

`PublicationUnknown` 不能简单释放票据让后续写入继续：先确认前一后端任务不会再晚发布，并通过校验/查询解除不确定，否则暂停该目标 lane 并报告。不能因为旧返回已超时，就允许它以后覆盖新的文件。取消 Publishing 的请求同样必须等到后端明确终态；不用强杀 worker 实现取消。

同一 Session 同一 binding 的后继保存可衔接前序版本；不同工作副本不得自动继承另一个会话的写入前提。冲突是结构化结果，不能静默 last-writer-wins。

### D2. 保存基线采用

检查 SessionId 代际、HistoryId、BindingRevision、已采用发布序号四项；不要求当前 StateId 等于捕获 StateId。S10 写完时当前 S12，则基线=S10、当前仍 dirty。后到的旧回执不能把基线从更新的发布结果退回。

底层已经确认名字替换成功、只是进一步持久性步骤失败，结果应为“已发布、持久性未确认”，不假装未写入。业务基线是否更新按本后端约定的发布保证；UI 必须显示准确警告而非再次无提示覆盖。

## E. Save As 的完整契约

1. 先阻止该 Session 新的 Save As/Reload/关闭提交和编辑，且等待/明确处理该会话已有保存。等待阶段仍驱动 owner 与完成，不能阻塞主线程。新普通 Save 被 admission 拒绝并说明理由，普通 Save 本身不冻结编辑。
2. 取得 BindingChangePermit，捕获稳定状态和新目标身份。构造 `EncodedSceneClone` 明确包含 artifact、package、identity_mapping；Material/Flow 对应输出各自的复制/重绑定数据。
3. 在写入之前准备 `PreparedSceneRebind`，包含新的 SourceBinding、BindingRevision、引用映射/角色更新和可无失败采用的状态；验证 undo memento 仍使用作者逻辑身份。需要分配的采用资源必须此时准备。
4. 写入使用同一 WriteLane，而不是一个绕过排序的 SaveAsWriter。新目标存在时使用明确覆盖/冲突策略。
5. 发布成功后 owner 只执行预备好的采用：更改来源与基线，保留作者 UUID、当前 HistoryId 和可用 undo。不能在成功写完后才发现不支持 rebind；格式确不支持时写入前返回 RebindUnsupported，仍允许明确的 Export Copy。
6. 失败保持原绑定、历史、dirty；PublicationUnknown 保留已编码产物与可核验目标，旧绑定不变。不得无限冻结用户工作副本；释放许可后若用户继续编辑，后来确认旧副本已写也不能自动采用旧候选，必须重新确认和验证。
7. Export Copy 写入副本不变更当前来源和保存基线。未来 compiled artifact 发布也不标记作者源 clean。

### E1. 对旧隐藏输出的处置

新路径禁止 `SceneSaveCapture::copied`、`SceneEncoder(const input)` 通过 shared_ptr 改外部状态；编码必须返回拥有结果。旧产品尚使用 TAssetSave 时该旧实现仅存在于旧 target，记录 P12 的删除项，不让它成为新 SceneEncodeJob 的依赖或作为新代码模板继续复制。

## F. 构建、真实 IO 与边界条件

`editor_persistence` 只依赖 contracts/sessions；`project_io` 实现存储接口；scene/material/flowforge_persistence 各依赖自己 model 与 persistence。加载工厂的轻量角色要到 P11 才挂入扩展，当前提供真实具体 decode→PreparedData→owner 构造函数与测试，不能在 worker 创建带线程亲和性的 History。

fake 存储必须能独立控制编码完成顺序、发布开始、发布后报错、未知结果、任务取消和完成采用顺序。真实临时目录测试必须覆盖发布/覆盖、旧文件保留、规范化地址、权限失败；不使用用户资产目录。

设置可配置的 max_active_saves、snapshot_bytes、terminal_records 等上界；超额在 admission 准确拒绝，不能悄悄丢弃未观察完成。未知发布记录只有可核验结果或明确用户决策后才释放其责任。进度展示可关联原 TaskId，不复制整套任务中心。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| 新编码路径中的可写 const 输入、live Session*/Pane* worker 捕获 | 禁止；全部用拥有快照与显式编码结果 | FrozenSave / EncodedArtifact / EncodedSceneClone；P05 | 已验证算法、资产兼容读取与行为回归不删 |
| TAssetSave / SceneSave / MaterialSave / FlowSave 与 SceneSaveCapture::copied | 仅旧产品限期保留；不得被新 target include；不扩展其功能 | SaveService 与各域 save source/codec；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| 已有 ProjectStorage、VFS/存储协议、ExecutionRuntime/stdexec | 复用并提供明确适配，不整体替换 | ProjectArtifactStore / 原执行器；保留 | 已验证算法、资产兼容读取与行为回归不删 |
| 旧工具 requestSave*/abandonSave/acknowledgeSave/retrySave 公共 API | 新调用方不使用；重试作为新准入或显式同操作阶段，语义明确 | SaveService；窗口端最终删除；P12 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X05-01 / `reverse_encoding_keeps_publish_order` | W1 慢/W2 快，随后 undo 到更旧状态再 Save | 磁盘最终对应最后准入意图；不按 StateId 序号排序 |
| X05-02 / `failed_or_cancelled_ticket_unblocks_lane` | W1 编码失败或确认取消，W2 已 ready | 队列前进，无 worker 同步等待导致饥饿 |
| X05-03 / `unknown_publish_does_not_race_successor` | W1 返回不确定且可能晚发布，W2 就绪 | 不允许 W2 被晚 W1 覆盖；lane 仅在责任结清后继续 |
| X05-04 / `old_saved_content_does_not_clean_current` | 捕获 S10 后编辑 S12，倒序投递回执 | 基线准确，无回退；新历史/绑定不被污染 |
| X05-05 / `save_as_keeps_history_and_origin_on_failure` | 新目标发布前失败、发布成功、发布不确定分别运行 | 失败原绑定/历史完整；成功保留 undo；不确定无自动错误采用 |
| X05-06 / `two_sessions_same_target_conflict` | 两副本以同基线经不同别名保存 | 同 lane，第二副本冲突不能静默继承前序版本 |
| X05-07 / `late_completion_after_session_removed` | 注销角色、删除 Session、复用槽后投递完成 | 仍记录磁盘事实，无 UAF/新对象污染 |
| X05-08 / `real_file_commit_reports_actual_fact` | 真实临时文件替换后使后续持久性/偏好步骤失败 | 报告已发布与后续失败，而不是未发生；原始证据保留 |
| X05-09 / `save_result_retention_is_bounded` | 持续保存直到容量阈值并 ack/cancel | 准入背压正确，无无限增长、提前释放或漏终态 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P05.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
