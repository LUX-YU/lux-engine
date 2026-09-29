# P04 R1 复审结论与 P05 启动指令

**日期：2026-09-29**  
**项目：LUX-YU/lux-engine**  
**实施分支：codex/editor-redesign-v4**  
**本次读取的验收 HEAD：`aad13c594afe33af69fcfba1b44790b4e9ad148d`**  
**P04 R1 实现提交：`af8310f362ca299a8e3b22597ce9cb5d735906a2`**

## 0. 决定、证据边界与文件地位

**P04 R1 在本次复审范围内通过，允许进入 P05；只授权 P05，不自动进入 P06，不修改 main。**

本轮审阅了远端 HEAD/提交父链、R1 实现差异、GraphTopology/FlowGraph 的身份保留实现、六类新增回归的关键源码、验收验证器，以及部分修复前后和 CTest/安装消费者日志。未发现需要继续阻止 P05 的前置缺口。

本轮没有独立构建引擎，没有运行完整 CTest、安装消费者、GPU、Android 或真实 DLL 卸载测试，也没有逐份复算全部远端证据哈希。88/88、八组 9/9 是实施方归档成绩；源码阅读与归档读取不等于第二套环境重新执行。附件缩小探针也不作为本轮真实引擎成绩。

本文是原 V4 `phases/P05_persistence.md` 的启动补充，不是另一版架构。下文 P05 内容属于**拟实施契约**，不是声称仓库已经有这些类型或行为。既有类型和目录以已验收代码为准；新类型的目标职责仍按原 P05。旧文档的 `docs/editor-redesign/`、根目录 `editor/history/`、`editor/sessions/` 示例已被实际交接决定取代。

启动包中的原 V4 仅是不可变历史参考，**不得将其 manifests、种子账本或审计脚本覆盖当前仓库**。先读取当前施工材料和各阶段收据，沿实际迁移记录定位符号。

## 1. P04 R1 复审摘要

### 1.1 两条真实失败已由针对性改动闭合

| 事项 | 已读证据 | 判断 |
|---|---|---|
| 混合插入、Undo、随后自动插入 | 原日志 NodeId 4 / PinId 8–11 重用，旧 Pin 写入被接受；新日志 NodeId 5 / PinId 12–15，旧 Pin 写入被拒绝 | 原缺陷的主路径已有真实前后对照；具体数字属于这次 Fixture，不应写成通用业务常量 |
| 从现存记录重建候选 | 在执行任何发号意图前由 live 图继承完整已发号状态 | 不是仅在提交时补救，避免候选已经发出旧号 |
| 提交及 Undo/Redo | live/parked 双向合并三类游标，再交换内容 | 内容可以回退，已公开身份不会随内容回退 |
| Pin 独立变化 | 签名扩展、Undo/收缩、新分支；既有位置 Pin 保留，新 Pin 不撞已发号集合 | 没有只测节点而遗漏 Pin |
| 耗尽 | topology 的 0 保持吸收；显式恢复较小 ID 不重开发号；变量 UINT64_MAX 维持原语义 | 不是普通 max 导致耗尽状态复活 |
| 拒绝与 NO_CHANGE | 未发布候选可丢弃内部临时号码，不伪造历史提交；失败后源/历史/基线不变 | 不把身份修复扩张成全局预分配器 |

生产责任仍在原来的 GraphTopology 节点/Pin 游标和 FlowGraph 变量游标中；未新增 Session 计数器、第二套 History 或 IdManager。`preserveVariableIdsFrom` 被完整替换。验证器按固定实现 Git 对象检查旧 API 活动源码残留，而不是删除历史文档中的原名称。[S1–S6]

### 1.2 可继承的验收与限制

归档 CTest 为 88/88，新增六个身份场景，原 82 项继续保留。安装后 Flow consumer 还实际执行了混合插入/Undo/旧 Pin 拒绝，不只验证能 include/link。附件与验收记录报告八组消费者 9/9。[S7–S8]

保持以下范围：同一 Session/History 域内已发号身份不回退；不扩张为跨文件、跨进程或新 History 的全球单调。正常 Redo 和显式恢复原身份仍合法。

C01（P09/P12）、C03（P11）、C04（P12）继续保留 FAIL。整图混合候选成本、现有 codec 种类、有限身份空间和已报告平台限制继续保留。本次不再增加 P04 修复任务。

## 2. P05 的交付目标与不做事项

**建立贯穿三类真实会话的完整持久化链，而不是把旧窗口保存函数再包一层。**

```text
owner 上请求准入 + 确定目标/预留写票据
    → owner 上捕获已提交内容
    → 拥有型输入跨异步边界编码
    → 统一协调器按目标顺序发布
    → 记录实际磁盘事实
    → owner 上尝试采用回执
    → 保留可查询、可确认的终态
```

P05 必须包括：三类编码/解码适配、普通 Save、Save As、Export Copy、有序发布、取消和不确定结果、迟到结果安全、实际 IO 验证与有界记录。不能只交付 mock 存储或单一 Flow 路径，再将 Scene/Material 留给后续。

P05 不包括：新 UI/菜单/布局流程，完整 Open/Close 产品用例，运行预览，Flow linker 或材质编译服务，执行器重写，通用工作流引擎。P07/P09/P11/P12 的实际生产者尚未出现，不得为了演示“共用写入协调器”提前实现它们；使用有限测试生产者证明接口共享即可。

旧产品可以暂时继续走已登记旧保存接线，P12 完成产品入口切换。P05 的同目标顺序保证只覆盖已经接入同一个协调器的生产者；**不得声称未迁移的旧写入器也自动受此保护**，测试中也不能让新旧写入器无协调地并写同一文件。

## 3. 起点与内部实施顺序

### 3.1 起点核对

以本次验收 HEAD 为前置祖先。若分支已有后续提交，解释差异，不 reset 到旧 SHA。保留用户修改，不 force-push，不改 main。

读取 `dev_log/P00` 到 `dev_log/P04-R1` 的收据、原 P05 和本补充。`.internal/editor-redesign/` 是唯一可变施工材料；本阶段结束冻结到 `dev_log/P05/`。历史验证按各自 implementation_sha 执行，不拿今天的文件路径推翻旧证据。

### 3.2 按以下顺序实施，阶段编号仍只有 P05

| 顺序 | 任务 | 必须先成立的行为 |
|---|---|---|
| P05-A | 核对现有 ProjectStorage、写入协议、执行完成分发；确定 Save/Publication/Adoption 的结果契约及容量上界 | 逐个明确哪个结果说明磁盘已改变，哪个结果只说明请求获准；记录当前文件发布与 durability 能力，不猜测 |
| P05-B | WriteTargetKey、WriteTicket、WriteCoordinator 与可控存储测试 | 先请求后发布；编码乱序不乱写；失败空洞、取消、不确定 lane 有确定语义 |
| P05-C | SaveService、注册撤销、FrozenSave/IEncodeJob 与真实项目 IO 适配 | 操作不借 live Session；注册失效和晚完成安全；真实文件发布事实可核对 |
| P05-D | Scene、Material、Flow 具体保存/解码适配与基线采用 | 同一已提交内容捕获后继续编辑仍可正确保存；解码值与 owner 构造分离 |
| P05-E | 三类 Save As、Export Copy 和预备 rebind | 发布前准备可采用状态；成功保留历史与作者身份，失败不破坏原绑定；不确定发布可释放编辑但不丢责任 |
| P05-F | 完整失败矩阵、独立安装消费者、实际依赖门禁、旧代码处置与冻结验收 | X05-01～09 和相关 Q 在最终 SHA 上全部核对；不能用前面局部开发成绩代替最终交付 |

这些是同一阶段内部的施工次序，可分实现提交，不是六个新增阶段。不必每个内部步骤等待再次授权；遇到真正的前置缺陷应记录实际影响，不用不断新增架构层绕过。

## 4. 类型与模块的准确归属

以下名称沿用原 P05；若已有等价类型，先论证并复用，不能为文档里每个角色再造同义类。小值归入同一语义头，不以一个枚举一个库的方式拆分。

| 模块 | 类型/角色 | 责任与关系 | 明确禁止 |
|---|---|---|---|
| `editor/persistence` / `editor_persistence` | SaveId、SaveRequest/Status/Outcome | 区分准入、阶段、磁盘结果、采用结果；SaveId 可识别过期记录 | 只用 bool saved/done；把任务 ID 当全部业务状态 |
| 同模块 | FrozenSave、EncodedArtifact、IEncodeJob | 拥有快照/字节；确有异构编码时用窄虚接口；外层 code owner 长于虚析构 | job 捕获 Session*、Pane*、临时 read view；const 输入写隐含输出 |
| 同模块 | ISaveSource、SaveSourceRegistration | owner 线程上的具体会话角色；注册 token 控制可调用性 | 在 IEditSession 加 save/compile/play 大接口；注册对象借已失效适配器 |
| 同模块 | SaveService、SaveOperation | Service 组合并拥有稳定操作；记录终态、取消和回执采用 | 通用 OperationManager；每个 operation 新建线程池 |
| 同模块 | WriteTargetKey/Target/Ticket/Lane、WriteCoordinator | 协调器唯一拥有 lane；按目标与准入顺序调度 | 每个 SaveSource/Save As 私建同文件队列；按 StateId 排序 |
| 同模块 | IArtifactStore、PublicationOutcome、CommitReceipt | 真实 IO 接口；Published/NotPublished/PublicationUnknown 与 durability 分开 | 超时=未写；写后附带步骤失败=未发布 |
| `editor/adapters/project_io` / `project_io` | ProjectArtifactStore final : IArtifactStore | 复用实际 ProjectStorage/存储能力；目标归一化与版本前提 | 在纯作者模型塞 ProjectStorage；重新发明 VFS |
| `editor/tools/scene/persistence` | SceneSaveSource、SceneEncodeJob、SceneCodec、ScenePersistenceAccess | composition/角色适配；冻结并显式输出包和映射，限制源/基线写权限 | 依赖旧 SceneEditor/SceneSaveCapture::copied；复用运行 Registry 作为保存真相 |
| `editor/tools/material/persistence` | MaterialSaveSource、MaterialEncodeJob、MaterialCodec | 保存材质作者源，不编译预览产物 | 保存成功=编译成功；把编译产物发布标成作者 clean |
| `editor/tools/flowforge/persistence` | FlowSaveSource、FlowEncodeJob、FlowCodec | 消费已冻结 FlowSource 拥有值，实际保存现有格式 | worker 遍历 live FlowGraph；调用 linker |
| 各工具 persistence | 具体 SaveAsOperation、PreparedRebind、EncodedClone/结果 | 组合 permit、编码结果、写票据和采用候选；领域格式独立 | 从通用 Session 继承；用私有 Reload 替代 rebind 后清空 History |
| 各工具 persistence | PreparedSceneData/MaterialData/FlowData | 拥有解码结果；在 owner 构造会话 | worker 创建带线程亲和性的 History、SessionState 或 SessionStore |

### 4.1 目录收敛规则

沿用 `editor/editing/history/`、`editor/editing/sessions/`，不重建 `editor/history/`、`editor/sessions/`。

坚持原 P05 的一个通用持久化主题、一个真实 IO 适配、各工具本地持久化子模块。WriteCoordinator、Lane、Ticket、SaveOperation 等都在同一持久化模块内部；不为每个类新增顶层目录、target 或 DLL。

本轮不再次搬动已有 storage/assets/project 目录。目录归组不等于取消依赖边界；构建 target 也不等于必须各出一个动态库。具体 STATIC/接口目标的选择应沿现有构建约定，记录真实安装闭包。

### 4.2 依赖方向

```text
三类作者 model → edit_sessions / edit_history / 自己的纯领域库
editor_persistence → contracts / sessions
project_io → editor_persistence + 已有真实存储能力
各工具 persistence → 自己 model + editor_persistence + 所需纯 codec
装配/测试 → 组合上述具体对象
```

实际执行器绑定复用现有设施；如纯核心不应直接带入执行运行时，可在窄调度适配中装配，而不是令 Session 或 History 反向依赖 SaveService。不能为配合表格假装实际链接没有执行器依赖；以实际 direct/transitive 闭包说明。

三类作者 model 不得反向依赖 persistence、ProjectStorage、UI 或编译服务。受限 access 必须是模型定义的窄协议/明确 friend，持久化消费者只能用支持的头；不开放整份 Impl，也不从兄弟模块偷用 src/pinclude/sinclude。

## 5. 必须先区分的三个成功事实

| 事实 | 表示什么 | 不能推出什么 |
|---|---|---|
| Accepted | 已获得 SaveId、容量与目标票据等准入资源 | 没有证明完成编码或写入 |
| Published | 后端确认指定字节已成为目标当前文件/对象版本 | 不自动等于达到全部崩溃持久性保证；不证明当前会话仍与该内容一致 |
| Applied | owner 已验证身份/绑定/发布顺序并更新对应保存基线 | 不一定使当前内容 clean；用户可能已编辑至更晚状态 |

公开 outcome 必须能同时表达 `Published + StaleBinding/Closed`，也能表达 `Published + durability未确认`。不要让采用失败覆盖掉已经发生的磁盘事实，也不要把登记/Task 提交成功写成保存成功。

服务终态与 lane 责任不同：PublicationUnknown 可以让用户获得明确的待核验状态，但其目标写入责任不能因 acknowledge 或终态记录裁剪就被丢弃。

## 6. 同目标顺序：必须在写入侧成立

### 6.1 准入顺序不是内容版本大小

W1 捕获 S10，随后 W2 捕获 S12。W2 编码先完成，也不能绕过 W1 发布。W1 写完再 W2；W1 编码失败或确认未发布取消则结清票据，W2 才能前进。

用户 Undo 回 S8 再 Save 是一个新的合法写入意图，应成为新的队尾；不能按 StateId.serial 大小把它当过时请求丢弃。ContentStamp 验内容，WriteTicket/PublicationOrder 表达写入顺序，两者不要混用。

回执乱序时，保存基线可以拒绝旧回执；但它不能修复文件已经被旧任务覆盖的问题。所以“在 accept 时丢弃旧结果”不是 WriteCoordinator 的替代品。

### 6.2 只有一个协调器拥有相同目标的写入顺序

应用装配/测试持有唯一 WriteCoordinator，SaveService 与具体 Save As 借用它。后续编译发布和 WorkspaceStore 必须接入同一实例；本轮不提前实现那些产品模块。

测试至少证明两个不同新生产者写同目标仍共用 lane；仅分别证明各自队列有序不够。同一路径的不同字符串形式应由真实后端规范化为相同 key。

声明大小写、相对路径、符号链接、尚不存在目标等支持范围。遇到无法保证身份等价的目标，应按明确支持策略处理，不虚称解决全部文件系统别名。未实现跨进程锁/CAS，只承诺本协调器内排序及已实现的外部冲突检测。

### 6.3 不确定结果不能让后继覆盖责任失控

Publish 返回 Unknown 或超时，不证明旧任务不会在未来再次写入。lane 保持隔离，直到后端确认前任务无未来写能力，并完成需要的状态核验。

只读一次文件发现内容正确，不足以证明仍运行的旧 writer 不会随后发布。不得强杀 worker、任意 sleep、将结果改成 NotPublished，或丢掉未知记录换取队列继续。

不同目标的 lane 可以继续工作；不要把一处 Unknown 扩张成全应用停摆。

### 6.4 两份工作副本不是同一条保存连续性

同一 Session/Binding 的后继保存可以在确认前序发布后衔接其版本前提。不同 Session 即使 Save 到同一文件，也不能自动继承前一会话的新版本并静默覆盖；必须报告冲突或执行已明确授权的覆盖策略。[V4-P05 D1]

## 7. 快照、生命周期、注册与迟到结果

### 7.1 捕获范围必须与业务身份一致

捕获 ContentStamp、BindingRevision、确定目标与冻结内容的过程应处于同一 owner 约束内；不能先拿目标，开放可重入窗口后再拿另一个绑定的内容。

所有回调编码/克隆、输入清理和异常展开继承现有 withRead/有序 owner 作用域；异步边界之外不能留下 SceneReadView、MaterialReadView、FlowReadView 或可写输入别名。

SceneSnapshot、MaterialSnapshot、FlowSnapshot 的拥有事实不同，不统一强行转换成活模型。FlowSnapshot 已是拥有 FlowSource 值，不为它机械增加不存在的节点 lease；Material/Scene 中确实需要的 code/deleter 寿命不能省略。

### 7.2 普通保存不锁住后续编辑

同步冻结完成后，应释放短读取准入，允许用户继续编辑；后台 job 只读自己的捕获。不能为省事把整个编码/写入阶段放进 READING 或 BindingChangePermit。

例：W1 保存 S10 时用户进入 S12。W1 完成后 checkpoint=S10，current=S12，仍 dirty；Undo 回 S10 可以重新 clean。不能因为保存请求成功清掉所有修改。

### 7.3 代码 owner 与 job 必须作为一个正确拥有单元

节点、编码 job、deferred deleter 和它们引用的元信息数据，必须先于最后 code owner 释放。外层拥有者成员声明次序、移动赋值和失败拒绝路径都要检查；不能依赖函数参数求值/析构顺序。

不要只在正常进入 lambda 后保护。准入失败、容量不足、注册过期、codec 抛错、取消和 service 清理时，未移交输入也必须安全销毁。已经取得 gate 时，清理不能跑到 gate 退出之后。

### 7.4 注销与 Session 销毁不等于抹掉磁盘事实

注册 token 可撤销，完成记录使用操作/注册代际 ID。调用 accept 前重新确认登记仍有效、SessionId 代际匹配。别捕获可能随容器移动或析构而失效的 operation this / raw adapter。

撤销后已经在途的写入仍可能完成。服务记录 Published/NotPublished/Unknown，但不能调用旧适配器，也不能把结果应用到复用槽位的新 Session。acknowledge 只处理可释放的观察记录，不取消已经开始的 IO。

核心验收是确实关闭旧对象、复用槽位、晚到真实拥有结果仍安全；不能通过永远持有整个 Session shared_ptr 或禁止关闭来躲避。

## 8. Save As 是重绑定，不是重载

### 8.1 必须保留什么

成功 Save As 保留 SessionId、当前 HistoryId、作者对象逻辑身份、Undo/Redo 可用性，以及同一作者域已发号高水位。改变的是目标绑定、BindingRevision、格式需要的外层资产身份/引用映射和已确认保存基线。

**不能调用 PreparedSceneReload / PreparedMaterialReload / PreparedFlowReload 来完成 Save As。** 这些内部候选用于新 History 的重载语义；以它们清空 History 可以让文件保存看似成功，却违背原 P05。[V4-P05 E]

尤其不要将保存后的图重新 materialize 再只恢复 current；P04 R1 刚确认的 Node/Pin/变量高水位和原历史 memento 也必须继续成立。对 Flow Save As 后再次 Undo/新建进行实际回归；显式重载到新 History 则仍按其独立语义处理。

### 8.2 按既定 BindingChangePermit 准备，不重新造 busy

先阻止本会话新的编辑、Save As、Reload 与关闭提交，非阻塞地排空或明确处理已在途的普通保存。不要在 owner 上同步等 worker，导致其完成回调没有机会被采用。

持有 BindingChangePermit 时不要先把 gate 恢复 AVAILABLE 再公开 capture；这会重新开放修改。可以在模型拥有者内部验证有效 permit 后执行受限的稳定捕获，也可以在同一 owner 协议中先完成捕获再取得并验证 permit；必须证明中间没有未验证内容/绑定变化。不要扩大普通 withRead 为任何 REBINDING 状态都可绕过。

需要的 capture/rebind 能力以窄 access 或实际有效许可表达，不给所有人公开 markClean/replaceSource/Impl&。

### 8.3 先准备采用，再发布

发布前完成目标覆盖策略、包内映射、SourceBinding 字符串/登记更新等可能失败的准备。编码返回 EncodedSceneClone/对应拥有结果，不能通过 const 输入的 shared_ptr 写 `copied`。

发布成功后只执行已经准备好的、不会再调用不受控外部逻辑的采用。将“磁盘已发布但采用失败”保留为真实结果，不能假装此前没写。格式本来不支持不破坏历史的重绑定，写入前明确 RebindUnsupported，并保留合法 Export Copy；不能以全类型恒返回 Unsupported 冒充 P05 完成。

### 8.4 失败、未知和 Export Copy

已确认发布前失败：原绑定、源、历史、dirty、checkpoint 不变，释放本次许可。

PublicationUnknown：原绑定暂不变化，写目标责任继续隔离。不能无限冻结用户；释放许可后用户可能继续编辑，后续即使确认旧副本已写，也不能自动套用过时 rebind 候选，必须重新确认与校验。

Export Copy：写副本，当前会话的绑定和 checkpoint 都不变；不能在共享普通 Save 完成分支时顺手 accept 成为当前保存点。

## 9. 解码、实际文件系统与容量

后台解码产物是独立拥有值 PreparedData，owner 再用原模型工厂构造 Session/History。Flow 元信息、Material 节点、Scene schema 所需环境与代码寿命要准确保活；不在 worker 临时创建 Session 后迁线程。

真实临时目录验证发布、覆盖、旧文件保留、别名归一化、权限/路径错误以及“已替换但后续步骤失败”。不使用用户项目资产做破坏性测试。故障注入必须落在实际执行路径，不能仅返回一个预设 PublicationOutcome 就声称真实 IO 通过。

配置 max_active_saves、snapshot_bytes、terminal_records 与未结责任容量；队列满时准入失败，不暗丢尚未观察终态。证据说明这是哪些内容/容器的计量，不虚称进程 RSS 硬上限。Unknown 责任必须留存或有明确转交，不能由自动裁剪丢失。

文件命名替换成功与断电持久性不是同一事实；采用现有后端能保证的语义，并在 outcome/测试说明中如实表达，不默认多文件事务或全局 CAS。

## 10. 旧代码处置与禁止残留

| 原系统/符号 | 本阶段处置 | 最迟期限/限制 |
|---|---|---|
| `editor/transition/LegacyPersistenceState.*` | 只供已登记旧工具，保持唯一兼容桥；新服务不依赖 | P12 删除，不扩消费者 |
| 旧 `TAssetSave` 与 `SceneSave/MaterialSave/FlowSave` | 旧产品未切换时可暂留；新保存链必须真正不依赖它们 | P12；不是新 EncodeJob 的模板 |
| `SceneSaveCapture::copied` 与旧隐藏编码输出 | 新路径严格禁止；旧 target 限期保留并记录实际旧消费者 | P12 删除；本轮提取的纯 codec 若重复须只留一份 |
| 旧工具 `requestSave*/retrySave/abandonSave/acknowledgeSave` | 新用例不调用；旧窗口临时接线继续保证原产品回归 | P12；不提前切换完整 UI |
| 原 codec、ProjectStorage、ExecutionRuntime/stdexec | 复用或提取已存在的纯子集，保留格式兼容与执行机制 | 不整体推翻、不新增线程池 |
| 三类 model 的保存采用/重绑定访问 | 在实际需要处增加窄访问契约；不得把 IO 反向装入模型 | 同步真实调用与行为测试；无空接口 |
| 已迁走或到期纯算法 | 原定义、声明、调用、生成输入和安装条目一起清除 | 不留 alias、备份后缀、注释整段或未构建旧实现 |

旧产品正在使用的适配与“新实现复制了旧纯算法”是两回事。对每个暂留项记录限定调用方、责任阶段和实际文件，不统一写一句“P12再清理”结束。

## 11. 测试映射：原九项，不能只看最后计数

| 原编号 | 必须观察的实际行为 | 本补充强调的连接点 |
|---|---|---|
| X05-01 | W2 编码先完成仍按准入顺序发布；Undo 到旧状态后的新保存能成为最后文件 | 不按 StateId.serial 排序；检查真实最终字节 |
| X05-02 | W1 编码失败或确认未发布取消后 W2 继续 | 不让 worker 阻塞等队首；输入与代码正确清理 |
| X05-03 | W1 不确定且可能晚发布，W2 不越过 | 后端任务静默终结/核验与 lane 隔离都成立；只核对当前文件不足够 |
| X05-04 | 捕获 S10 后编辑 S12，倒序交付回执 | checkpoint 正确，重载/重绑定身份不被旧回执污染 |
| X05-05 | 三类 Save As 的失败、成功、未知；Export Copy | 成功保留历史/作者身份/高水位；失败不半采用；不使用 Reload 清空历史 |
| X05-06 | 两会话同基线经同目标别名写入 | 真正共用 lane；第二副本不能静默承接第一副本的新版本 |
| X05-07 | 注销适配、删除并复用 Session 槽后完成 | 磁盘事实保留；不调用旧适配，不污染新代际 |
| X05-08 | 真实文件替换后进一步步骤失败 | 报告已发布与附带失败，不能报告未写；非用户目录运行 |
| X05-09 | 到容量阈值、ack、cancel、Unknown | 明确背压、无丢终态、无提前释放、未结责任仍受控 |

这些测试语义来自原 P05；本补充不另建一套编号系统。三类实际模型必须进入保存验证，不能只用 fake ISaveSource 验全阶段。可控 fake 用于精确乱序/取消/未知调度，真实 IO 用于文件发布事实，两者互补。

同时保留 P01/P02/P03/P04/R1、88 项已有 CTest 及原断言。测试数量允许因真实新场景增加；删除、合并或改名必须说明语义覆盖，不靠总数漂漂亮亮就声称保留。

相关 V3 Q11～Q18、Q50 按实际 P05 责任映射。安装后消费者真实构造模型、保存/失败、检查字节和基线；不从源码构建目录借 DLL。已有八组九项继续回归，新持久化消费者按实际包边界登记，不为凑组数新建库。

依赖负例核对通用持久化不依赖具体 model/UI，model 不反向依赖持久化/真实 IO，项目 IO 不绕进旧 Editor，三域适配不偷 include 旧 Impl。真实 CMake 和 include 夹具应拒绝指定规则，修复同夹具后通过，不能因缺包误算成功。

## 12. 阶段末交付与允许停点

最终显式 `LUX_EDITOR_MIGRATION_STAGE=P05`，绑定一个确切实现 SHA 执行完整构建、第二轮、CTest、SDK 安装与受影响消费者；必要 modules 公共头变更按现有约定同步，不将仅同步头称作 Android 构建。

实现与验收分开提交；`dev_log/P05/` 至少冻结：收据、文件清单、当前迁移账本/架构/覆盖表、真实命令、可取得归档与哈希、未运行项、原缺陷结果。source 已变化时历史核验使用其原 implementation_sha。

报告特别列出：

1. SaveId/registration/operation、WriteCoordinator/lane、快照/job 的唯一 owner 和终结次序。
2. Published、NotPublished、Unknown、adoption 的实际保证，版本冲突范围及未实现的文件系统特性。
3. 三类 Save As 的历史和身份保持证据；Export Copy 不改变保存基线。
4. 旧纯逻辑迁出/删除、必要旧适配消费者与期限；新路径没有旧隐藏输出和 worker live 借用。
5. 九项 X05、相关 Q、已有回归、安装及依赖验证逐项结果，不用一个总 PASS 替代。

未通过必测或缺失环境只能报告 PARTIAL/BLOCKED，并停在 P05。不得用旧 88/88 代替新阶段验收。C01、C03、C04 按原责任保留 FAIL，不顺手修，也不能把新持久化缺陷挂到它们延期。

## 13. 可直接发送给实施方

> P04 R1 复审通过，允许进入 P05，仅执行 P05。以 `aad13c594afe33af69fcfba1b44790b4e9ad148d` 为已验收前置，核对工作树和祖先关系，不 reset、不修改 main。
>
> 读取原 P05、本启动补充和当前施工账本；按 P05-A～F 内部顺序完成统一写入协调、保存服务、真实 IO、三类具体适配及 Save As，不把内部步骤扩成新阶段。沿用已收拢目录，不恢复旧 history/sessions 根目录。
>
> 严格区分准入、磁盘发布、基线采用。所有新写入生产者借同一 WriteCoordinator；编码乱序不能引起发布乱序，Unknown 不放走 lane。普通保存冻结后允许继续编辑，后台只拥有快照和结果，不持 live Session/Pane/read view。
>
> Save As 发布前准备 rebind，成功保留 History、作者身份和同域高水位；失败/未知不破坏原绑定。不得以 Reload、新会话或 clearHistory 规避。注册撤销、槽位复用、迟到结果、输入清理和最后代码 owner 寿命继承已验收契约。
>
> 执行原 X05-01～09、相关 Q、既有模型/R1回归、实际 IO、实际依赖负例和安装消费者。最终显式 P05 门禁；只通过 mock 或只完成一种模型不可报 PASS。旧产品适配按既定范围最迟 P12 删除，新路径不得依赖它们。
>
> 实现与验收分别提交，归档可移植证据。C01/C03/C04 保留原 FAIL 及责任。正常推送实施分支，停在 P05 等待复审，不自动进入 P06。

## 14. 固定来源

以下为本次复审已读取的固定源码/证据入口；P05 新设计以原 V4 P05 文件为规范依据。本文没有新增外部技术研究结论。

- [S1：实现提交差异](https://github.com/LUX-YU/lux-engine/commit/af8310f362ca299a8e3b22597ce9cb5d735906a2)
- [S2：GraphTopology 游标合并与推进](https://github.com/LUX-YU/lux-engine/blob/aad13c594afe33af69fcfba1b44790b4e9ad148d/modules/function/graph/src/GraphTopology.cpp)
- [S3：FlowGraph 三类身份保留](https://github.com/LUX-YU/lux-engine/blob/aad13c594afe33af69fcfba1b44790b4e9ad148d/modules/function/flowforge/include/lux/engine/flowforge/graph/FlowGraph.hpp)
- [S4：真实身份回归](https://github.com/LUX-YU/lux-engine/blob/aad13c594afe33af69fcfba1b44790b4e9ad148d/editor/tools/flowforge/model/test/flow_session.cpp)
- [S5：修复前 Undo 分支](https://github.com/LUX-YU/lux-engine/blob/aad13c594afe33af69fcfba1b44790b4e9ad148d/dev_log/P04-R1/before/ids-undo-branch.log)
- [S6：修复后 Undo 分支](https://github.com/LUX-YU/lux-engine/blob/aad13c594afe33af69fcfba1b44790b4e9ad148d/dev_log/P04-R1/logs/flow-ids-undo-branch.log)
- [S7：88 项 CTest 归档](https://github.com/LUX-YU/lux-engine/blob/aad13c594afe33af69fcfba1b44790b4e9ad148d/dev_log/P04-R1/logs/ctest.log)
- [S8：安装后 Flow 回归](https://github.com/LUX-YU/lux-engine/blob/aad13c594afe33af69fcfba1b44790b4e9ad148d/dev_log/P04-R1/logs/flow-installed-detail.log)
- [S9：R1 归档核验脚本](https://github.com/LUX-YU/lux-engine/blob/aad13c594afe33af69fcfba1b44790b4e9ad148d/dev_log/P04-R1/check_receipt.py)
- V4-P05：当前会话已提供的 `phases/P05_persistence.md` 全文；启动包按原相对路径保留原件，内容未改写。
- 附件 `README(6).md`：本轮 P04 R1 验收说明，用于与远端实现和日志对照；其中未独立复核的工程成绩仍按实施方归档表述。
