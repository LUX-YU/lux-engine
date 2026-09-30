# P08 R1 复审结论与 P09 启动指令

**日期：2026-09-30**  
**仓库：LUX-YU/lux-engine**  
**实施分支：`codex/editor-redesign-v4`**  
**本次已审阅验收 HEAD：`41167a9bdff8c192fe990d53aa8dfa132d57b081`**  
**P08 R1 实现：`48ebdb9d8082e391f9c12619ff93934b4ee330c2`**  
**原 P08 验收：`8f210a282109ec9e0dae3f32f88bc04359ef2c65`**

## 0. 放行结论、证据范围与文档优先级

**P08 R1 在本次源码、测试源码及已提交证据审阅范围内通过。允许进入 P09，仅执行 P09；结束后停下复审，不进入 P10，不修改 main。**

本次读取了远端分支和父链、修正提交、三类 interaction 的实际 cancel/synchronize、真实三模型回归、修复前后 Material 日志、CTest 汇总、两项显式 GPU 模式日志、验收说明与检查器，并读取原 V4 P09 全文。[S1–S9、P09]

**没有独立执行本轮引擎构建、160 项 CTest、PLAYER、SDK、GPU、编译负例，也没有执行整个归档检查器或逐份复算远端哈希。没有新增独立运行成绩。** 以下实测数字属于实施方归档，不能改写成复审者独立跑过的结果。

本文件第 1–2 节记录本轮复审；第 3 节以后是原 P09 的实施展开。表中的 P09 新类型是目标设计，不表示它们已经存在。没有把旧格式、构建脚本或当前后端的未验证能力当作既定事实。

优先顺序：本启动补充和历次已明确采纳的边界 → 当前实际实现及施工账本 → 原 P09 的业务要求和测试 → 原 V4 的历史路径与接口草图。原 V4 逐字保留；不得拿其中种子账本、旧路径、旧脚本覆盖已经推进至 P08 R1 的仓库。

## 1. P08 R1 为什么可以放行

### 1.1 临时失败不再触发失效清理

三类 interaction 的 cancel 均先检查 typed read：只有 `ESessionError::STALE_SESSION` 才进入无活会话借用的旧身份清理。BUSY、WRONG_THREAD 等原错误直接向调用方返回，在此之前不 reset 手势。[S3]

三类 synchronize 同样在 describe 之后先分类错误；非 STALE_SESSION 的失败直接返回，不再走笼统 `!info` 清理。因此，不存在手势而只有选择的情况也受到保护。原对象删除、History 换代、真正关闭和代际失效路径仍然保留。

未增加失效缓存、forceRead、第二个 busy 或新的交互管理器；没有放宽 SessionStore 的 reclaiming 保护。正确错误分类由原调用者承担。

### 1.2 活会话的清理仍在唯一 gate 内

正常取消将临时批次移动到 withRead 回调的局部对象，先 reset 活动手势，再在该回调退出前销毁局部批次。载荷析构看到的是“手势已经解除”，且 Session 的读取准入仍然持有。[S3]

确认 STALE_SESSION 后的清理也先移出批次、解除活动状态，不借用复用槽位中的新会话。这里延续的是已有对象身份契约，不是任意跨线程析构或插件 DLL 卸载认证。

### 1.3 新测试覆盖真实 Store 的回收边界

`editor/tests/persistence/interaction_reclaim.cpp` 用同一真实 SessionStore 中 B 的最后 CodeLease owner 清理触发对仍存活 A 的公开 interaction 调用；没有修改 Store 私有状态。[S4]

| 场景 | 已读取的实际断言 |
|---|---|
| sync | Store 确认 BUSY；interaction 报 BUSY，保留选择、载荷地址、起始戳和 label；回收后可同步、提交一条历史、Undo/Redo |
| cancel | BUSY 时零释放；安全点取消只释放一次，gate 仍持有且活动手势已解除；选择保留 |
| selection | 无手势也保留选择并报告 BUSY；真正领域删除后才剔除 |
| stale | 关闭旧会话，复用相同 slot 但新 generation；旧 interaction 清理自身，不能改新会话 |
| gate/thread | 同会话读取中拒绝取消/同步；错误线程仍返回原错误；状态不变 |
| reload | native 使用真实候选重载，同对象身份但新 History；旧手势/选择清理，当前源不受影响 |

native 分支进一步检查 HistorySnapshot、收费和 checkpoint；安装分支使用公共头/库、完整冻结编码、公开 SessionInfo 与后续领域操作验证。源码中读取依赖的差异已经通过编译条件明确区分，不把私有测试入口冒充 SDK API。

已读取的 Material 前后对照：[S5]

```text
修复前：store_busy=1 live=1 result_ok=1 reported_busy=0
        overlay=0 selection=0 stamp_payload_retained=0 released=1
修复后：store_busy=1 live=1 result_ok=0 reported_busy=1
        overlay=1 selection=1 stamp_payload_retained=1 released=0
        retry=sync PASS
```

原运行报告的作者内容未改变结论继续成立；修正的是有效交互状态丢失及错误成功返回，不夸大成已证明的文件损坏。

### 1.4 范围保持有限

实现文件清单为一个新增三模型测试源、八个修改文件，其中生产行为修改限于三个 interaction CPP。其余是同一组测试/SDK CMake 和契约说明。[S6]

没有公共头、生产 target、包名或目录变更。测试放在已有共享测试区，并不意味着生产 interaction 反向依赖 persistence。后续若整理测试目录，应作为明确的工程维护，不把它变成 P09 的新增前置任务。

## 2. 必须准确传递的验收事实

### 2.1 成绩分开计数

实施方归档为 Editor **160/160**（原 139 项加 21 项）、PLAYER **11/11**、原十二组 SDK **74/74**（原 56 项加 18 项），其中保留八项 operation 编译负例。另有 **GPU_UI、EDITOR_SCENE_PANE 两个显式模式**，不并入 74 的旧统计后再重复计数。[S2、S7、S8]

检查器要求精确新增测试集合、原 C++ 测试体不变、原依赖负例、九份修复前输出以及两项显式 CONSUMER_MODE。阅读脚本不等于本次独立运行它。[S9]

### 2.2 更正 GPU 证据口径，而不是改写历史文件

本轮报告发现：旧资格脚本的 `--fresh` 没传 CONSUMER_MODE，两个以 GPU/ScenePane 命名的消费者目录实际运行默认 CPU_UI。那些 CPU 通过日志不能被归类为 GPU 通过。[S2]

本轮另显式配置同一安装消费者：

- GPU_UI 日志记录实际 `feature.exe`、三帧 GPU、资源退休；没有像素读回声明。
- EDITOR_SCENE_PANE 日志记录 `scene_panes.exe --scene-panes`、12 帧 GPU、缩放与退休相关检查。

这修正的是特定两组消费者的证据分类，不能据此断言此前全部 Editor GPU 测试都未执行。也不能把这两项旧产品回归升级成 P10/P13 的完整新产品双视图像素、布局、IME 或 GPU 退休资格。[S8]

后续验收必须记录模式参数、有效配置和实际 test command/可执行文件；不以目录名、标签或消费者组名判断所跑行为。显式模式检查已经加入本轮门禁，P09 必须继承，不能只恢复旧资格脚本的默认参数。

### 2.3 历史、用户工作区与前置缺陷

`.internal/editor-redesign/` 是唯一可变施工材料；阶段末冻结到 `dev_log/P09/`。不要重新启用原 V4 的另一套 `docs/editor-redesign/receipts/` 活动账本。

`ProjectBuilder.cpp` 的既有修改按报告仍以 SHA256 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c` 保留，未纳入资格提交。本次未访问用户机器；P09 开始时再次核对，不 reset，不覆盖。

原冷构建失败及 P06 Physics2D 窄修复保持；本轮是干净源码、复用构建树，不宣称首次冷构建通过。没有 Android 构建。

C01 仍归 P09/P12；C03 仍归 P11；C04 仍归 P12。**P09 要完成 C01 的新纯验证/计划责任，但旧产品完整布局应用尚未切换，旧 C01 探针可以继续 FAIL，不能因为新 planner 通过而改写旧结果。** C02 对应的新存储一致性要求则须在 P09 新路径实际完成。新问题不得挂原编号延期。

## 3. P09 的目标：四类事实分开，组合在用例层

原 P09 的目标是“布局、会话恢复与偏好的模型和持久化”。它完成纯计划与真实 IO，不直接修改活动 Root；完整桌面接管和 ApplyLayout 在 P10/P12。[P09]

| 事实 | 内容 | 明确不包含 |
|---|---|---|
| 布局 | LayoutId、label、视图槽位、DockTree、纯视图配置 | 作者源、运行实例、live SessionId、打开资产动作 |
| 恢复清单 | 可持久内容 locator 与视图绑定意图 | 未持久化内容副本、自动保存保证、当前运行指针 |
| 用户偏好 | 选中的稳定 LayoutId 等偏好 | 布局正文、保存任务终态、活动视图 owner |
| 布局目录 | 可查询 LayoutSummary 与版本/诊断 | 请求、action、pending/result 混合状态袋 |

这些可以共同服务一个“工作区”产品体验，但不能重新合成 WorkspaceData/WorkspaceRequest 一种公共类型。

```text
布局文件/恢复清单/偏好
       │ 各自解析、预算与校验
       ▼
ValidatedLayout + ViewInfo 快照 + 固定 provider 描述
       │ 纯 resolve
       ▼
LayoutPlan：复用／需创建的未绑定外壳／保留额外视图／未知项
       │ P10/P12 后续使用；P09 不触碰 Root

WorkspaceStore ──借用──> 既有 WriteCoordinator
       └──────────────> 既有可靠文件发布基础／明确规范化后端
```

继承 P08 R1 的区别：读失败不等于不存在。目录读取权限失败、临时 BUSY 或 IO 错误，不能被转换成空目录并成功覆盖默认布局；记录缺失、格式损坏、未知版本和 provider 缺失也不能混为一类。

## 4. 当前源码起点与本轮新增设计的区别

本次读取了仍服务旧产品的 `EditorWorkspaceStorage.cpp`：[S10]

- 路径为 `.lux/editor/layouts/<name>.toml`，偏好文件为 `settings.toml`。
- 布局 v1 包含 panes 的 type/id/visible/payload、child windows 可见性和 dock 字符串。
- rename 先重命名布局文件，再写 selected 偏好；remove 先删除文件，再写偏好；目录刷新也在之后。
- 旧 write 使用 `.next` 后替换。新模块不能复制这一整条写入协议来形成第二个 WorkspaceWriter。

以上是旧格式/旧控制流事实，不代表新 WorkspaceStore 应保留同样责任边界。旧兼容路径到 P12 再退出；迁移器只读取格式，不 include 旧业务类型。

本次还核对 `ProjectArtifactStore`：[S11]

- resolve 使用既有 `publicationTargetKey` 和文件摘要形成 WriteTarget。
- publish 做前提版本验证、暂存写入、替换和独立持久性确认。
- 目前公开面只有 resolve/publish/reconcile；**没有据此确认一个已经可调用的“协调删除”接口。**

因此 P09-A 必须先明确 RemoveLayout 的真实删除契约及其与在途写入的关系，不能虚构已有方法，也不能直接 unlink 后假装已经受写协调器保护。只允许为该真实用例做最小必要适配；不能顺便引入通用事务/文件管理框架。

## 5. 替代类型、组合关系、目录与依赖

下表继承原 P09 的角色，按实际已有等价类型复用；没有等价类型才新增。类型角色不是一个 DLL，也不是要求每项有独立文件。

| 类型/角色 | 关系与 owner | 接口应表达的事实 | 归属 |
|---|---|---|---|
| LayoutId、LayoutSlotId | 可复制持久身份值 | label 改变不改变 LayoutId；slot 不是 live ViewId | workspace/layout |
| DockLayout、DockTree | 拥有纯值组合 | 完整布局、节点/槽位引用、版本及限额 | workspace/layout |
| ValidatedLayout | 验证器构造的有效值或窄封装 | 验证后不允许任意可写别名破坏不变量 | workspace/layout |
| LayoutPlanner、LayoutPlan | 具体纯规划功能和拥有输出值 | 只计算计划，不能 create/mount/open/rebind | workspace/layout |
| LayoutCatalog、LayoutSummary、CatalogVersion | 查询快照 | 显示目录及可观察问题，不驱动 IO 状态机 | workspace/layout |
| VersionedViewState、PreservedOpaqueState | 版本化自有字节及元信息 | 未知载荷可往返；不执行未知 provider | workspace/layout |
| UserPreferences | 独立值 | 稳定布局选择及诊断；与布局提交分开 | workspace/layout |
| RecoveryManifest、RecoveryEntry | 独立恢复描述 | locator、恢复关联、不可恢复修改的明确报告 | workspace/recovery |
| WorkspaceStore | 具体 IO 适配组合；借用协调器/可靠后端 | read/list/save/rename/remove 各自结果 | workspace/storage |
| LayoutCommitReceipt、PreferenceWriteResult | 拥有型结果 | 发生的文件副作用与偏好结果分别可观察 | workspace/storage |
| LegacyWorkspaceImporter | 私有格式适配，不是全局服务 owner | 最小旧 schema、稳定映射、只读备份和幂等迁移 | workspace/storage/src |

保持一个 `editor/workspace/` 功能主题；layout/recovery 可按纯值依赖共同编入一个适当目标，storage 维持真实 IO 的边界，具体沿当前 architecture.json 落地。不要给每种 ID、receipt 或 importer 再加 target。

原 UI 已有名为 DockLayout 的布局入口，不能因名字相同就当作新的纯持久 DockTree 模型。新类型使用明确 namespace；是否复用已有表示必须有等价语义证据，不保留两种互相冒充的公共协议。

新的纯布局/恢复值不依赖具体作者 Session、SceneRuntime、ProjectStorage 实现或旧 Context。planner 可使用 P08 ViewInfo、ViewType、ViewRestoreKey 的值，不借活动 Pane 指针；storage 显式依赖 editor_persistence 并复用既有可靠发布能力。

## 6. P09 内部顺序（一个阶段，A–F 连续实施）

### P09-A：先写清数据格式、当前后端能力和真实消费者

盘点旧布局编码、dock 几何、窗口 type/id、payload 中可识别的内容 locator、selected 偏好，以及对应私有 schema/版本。记录哪些信息能转换，哪些必须保持 opaque，哪些根本没有持久化。

确认 P08 实际 ViewInfo 的身份契约：ViewId 是临时 Host 域/slot/generation；ViewRestoreKey 是持久匹配键；ViewTypeId 复用 PaneTypeId；原 PaneId 不是恢复键也不是运行句柄。不要因恢复需要稳定 ID 就复用 live 代际句柄。

明确受控存储根、LayoutId 文件命名、读取与发布限额、目录部分损坏报告、同目标协调以及删除的已发生事实。编码选择沿用可维护工具，不升级 C++ 标准或引入全新序列化框架。把选择写入一次简短 ADR，不能同时维护新旧两个可写格式。

### P09-B：实现纯值、完整验证和无副作用计划

按 parse → validate → resolve 顺序处理全树：schema、重复身份、引用完整性、环、深度/数量、split 比例的有限性和范围、视图类型约束。

`ValidatedLayout` 不得由默认构造或 unchecked 可写数据任意伪造。计划返回自有数据或有明确 owner 的不可变引用；不得悬挂于 parser 缓冲、临时 provider 数组或下一次目录刷新。

匹配严格使用 ViewRestoreKey + ViewType。不同 view 类型不能同 key 误用；同类型也不能拿第一个现存视图重新绑定。额外现存视图默认保留，计划不关闭 dirty Session、不取消正在进行的交互，不调用 asset open。缺少 provider 时生成明确的未知/遗漏项并保留载荷；P09 不执行其任意 restore 回调。

若计划包含当前 ViewId，仅作为未来采用时重验的运行快照；不得序列化进布局文件，未来 P10/P12 不能把旧计划中的句柄当永久身份。

### P09-C：真实文件发布与结果语义

按原 P09 使用 `layouts/<LayoutId>.layout`，label 位于记录正文；RenameLayout 是对同一个记录的内容更新，不是改名文件后再修补 selected。偏好仅引用稳定 LayoutId。

save/rename/remove/read/list 分开方法或小命令，返回对应值或 receipt。结果至少能保留：布局操作实际是否发生、偏好写入是否发生、目录刷新是否成功。某项后处理失败不能擦除已经发生的前序事实，也不能把已发生操作盲目重试。

WorkspaceStore 借用 P05 同一个 WriteCoordinator，不私自创建副本。命名域只组织受控目录；规范化后的同一物理目标必须得到同一协调身份，不能靠不同 address 前缀绕开串行/冲突。复用当前已验证能力；不宣称任意外部多进程/硬链接并发协议已全面认证。

写入保留原请求版本、FIFO、Unknown 及 writer 退休规则。Already-accepted 的完成不能因业务 BUSY 丢失；读取/结果确认不要执行任意 provider。额外操作记录只为确实跨时刻的工作建立，不能为同步纯函数制造完整异步框架。

**RemoveLayout 必须覆盖同目标旧写入尚未完成的情况。** 删除成功后不能让早先的保存晚到又复活记录；不能通过取消请求已发出就假设旧 writer 已退休。先核对当前协调接口，再实现必要的有界删除串行/适配和明确结果。不伪装成 SaveService 的源码保存，不使用一个永远存在的空文件冒充已经完成的物理删除而不在格式/接口中说明。

真实 IO 测试至少检查磁盘正文、文件身份/路径、偏好残留及重启后的读取；仅记录模拟后端调用次数不足以通过原 X09。

### P09-D：恢复清单、偏好和回退

RecoveryManifest 保存可持久 locator 与视图关联，不保存 live Session 指针或将 dirty 位当作未保存数据。无法从仅有 locator 恢复未持久化修改时，明确报告；本阶段不新增自动保存系统。

用户选择的 LayoutId 确认不存在、或布局确已损坏时，采用有诊断的默认/安全回退，避免因单一偏好引用让项目无法启动。**暂时 BUSY、权限拒绝、目录访问失败，不得冒充“目录为空/布局不存在”。** 可以展示有来源标识的临时默认，但不因此成功写回并覆盖仍未读取的原数据。

list 中个别记录损坏，按定义返回逐项诊断或明确整次失败；不能默默丢掉记录后宣称是完整目录。回退、删除、刷新和偏好结果的状态不是同一枚 boolean。

### P09-E：旧格式只读迁移与幂等

仅解析旧版本的数据；已识别几何变成 DockLayout，内容 locator 进入独立恢复清单，旧 selected 转为新稳定 LayoutId。未知 provider/schema/字段保留必要 type/schema 和原始 payload。不能通过调用旧 Pane restore 获得解析结果，也不能 reinterpret_cast 到新类型。

稳定映射必须在反复启动、中断和重新扫描时重用；不能每次导入随机生成新 LayoutId。新文件写入后验证，再发布迁移完成标记；在两者之间中断，下次继续不能重复产生布局或覆盖已经迁移后被用户更新的新记录。

如果映射/已有目标不一致，明确冲突，不擅自把一个新记录当作这次导入产物。旧文件保持只读备份；布局数据、opaque 字节、映射和 before 负例不是“垃圾代码”。新路径只写新格式，旧业务 writer 只服务未切换的旧产品，两者不能被同时接到同一个新用例。

unknown 合法载荷按预算原样往返；超限准确拒绝，不能截断后标成完整成功。未来不认识的整个文件版本应有明确保留/拒绝策略，不强行降级重写。

### P09-F：迁移边界与阶段验收

实现纯计划和真实 WorkspaceStore，不在 P09 搭完整桌面 ViewHost，不重新实现 P08 的 Root 准备/提交，不提前完成 P12 ApplyLayout。旧 EditorWorkspaceStorage/EditorWorkspace 仅保留既有消费者，最迟 P12 删除。

对于原 C01，建立新验证拒绝坏 dock 的证据及“任何活动 UI/Session 未改”的检查；旧产品 C01 测试按原路径继续记录，不能以新方法取代旧失败测试后宣称整个问题关闭。C03/C04 也不顺手修复。

新模块禁止依赖旧 WorkspaceRequest/EWorkspaceAction/WorkspaceData/PaneManager/EditorContext；私有 importer 只保留格式结构。允许复用可靠工具函数，不允许照搬旧业务 owner。更新实际依赖检查、安装与消费者、现行迁移账本和期限；不用原 V4 的旧路径覆盖已收拢结构。

## 7. 原六项 X09：观察与失败分类

| 编号 | 必须实际观察 | 不能作为替代 |
|---|---|---|
| X09-01 | 有完整视图条目但 dock 坏、ID 重复或有环；纯验证失败；活动 UI/Session 计数和内容不变 | 先创建 Pane 再回滚、只检查 parser return |
| X09-02 | label 更新成功，偏好确实无法写；LayoutId/文件路径稳定；返回分开的真实结果 | 用 bool false 抹掉布局更新事实 |
| X09-03 | 删除成功但偏好仍引用旧 ID；重新创建读取入口后有诊断地回退 | 把权限错误当不存在、只改内存 selected |
| X09-04 | 合法未知类型和未来 payload 往返原字节；超预算/深度明确拒绝 | provider 缺失即丢 payload、截断后成功 |
| X09-05 | 新记录已发布但标记前中断，重启幂等；原文件不破坏，新 ID 不重复 | 在同一内存对象里模拟重启、每次随机 ID |
| X09-06 | dirty 内容、同类型不同恢复键、额外视图；精确复用、额外保留、零 open/rebind | 仅查同类型第一个窗口，或清理 dirty 来简化计划 |

这些场景的深化仍归原编号：目录不可达与不存在区分、同目标写/删排序、不同 namespace 同物理目标、迁移碰撞和中断恢复、非法计划身份、读取预算和通知后处理结果。不得为深化测试改变原阶段范围或新增另一套测试框架。

Windows 上“只读文件”是否真正拒绝写入需用实际返回验证；故障注入可以是可控后端拒绝，但必须另有真实文件副作用测试，并将两类证据分开。不要因环境权限未按预期拒绝而修改断言或谎报已覆盖。

## 8. 删除／暂留／保护清单

| 项目 | 本轮处理 |
|---|---|
| 新路径 WorkspaceRequest、EWorkspaceAction、WorkspaceData 的依赖 | 禁止；用独立值、结果和用例操作替代 |
| 旧 EditorWorkspace.cpp、EditorWorkspaceStorage.cpp、EditorWorkspace.hpp | 限定旧产品暂留到 P12；不能提前删掉仍承载功能的整文件 |
| importer 中拷入的旧 Context/Pane/restore 行为 | 不引入；只保留最小私有旧 schema |
| 第二份 `.next`/rename 写入协议、私有 WorkspaceWriter | 不引入；共用可靠发布与协调边界 |
| 每条新布局记录上的 current/pending/result/action 状态袋 | 不引入；运行操作状态与数据值分开 |
| 旧格式备份、稳定映射、unknown payload、失败证据 | 保留；不能误当历史垃圾删除 |
| P08 R1 临时 BUSY 保留契约 | 保留回归；不能为列表刷新或应用布局强行清理交互 |
| P07 R1 operation 特殊成员、P05 完成接收与 P06 退休结果 | 保留；本轮不重做原算法 |
| Root/Pane/Element 的离树及安全点 | 作为后续消费接口，不在纯 planner 里直接操作 |

符号迁移覆盖声明、定义、调用、friend、生成输入、CMake/安装、示例和活动测试；不以 `.old`、`#if 0`、空 alias 代替删除。明确语义相同的既有 ID 复用不属于兼容壳，不能为了数量再包装一次。

## 9. 验收门禁与证据交接

最终显式 `LUX_EDITOR_MIGRATION_STAGE=P09`。保留原 160 项行为和相关断言、PLAYER 11 项、原十二组 SDK 74 项、八项 operation 编译负例，并继续执行新增显式 GPU_UI 与 EDITOR_SCENE_PANE 两模式。数量可以因新测试增长，不能靠删除旧行为维持通过。

依赖负例至少覆盖：纯布局/恢复不依赖作者/Runtime/旧 Context；planner 不借 Root/Pane 或 IO 实现；新存储不 include 旧业务协议；Engine 不反向依赖 Editor；同物理目标不能绕过协调器。每个禁边应失败在预期规则，同夹具修复后通过，不能缺包就当架构检查有效。

构建、CTest、GPU 和 SDK 按既有规程顺序执行；SDK 重装后使用安装头/库。模式参数和实际命令必须归档，名称不是模式证明。公共 modules 头实际变更才触发相应安装前缀一致性检查，不做没有发生的 Android 资格声明。

本轮不要求新增全仓冷构建资格，但原风险与有限修复记录继续保留；更不能以“独立干净检出”代替冷构建事实。

验收收据至少含：准确 implementation_sha、六项 X09 与相关 Q、实际 IO/迁移中断记录、unknown 字节对照、结果语义、协调器实际实例来源和目标归一规则、当前/旧业务消费者、删除期限、SDK 及真实模式。原历史快照按其实现 SHA 验证。

状态只允许 PASS/PARTIAL/BLOCKED。新存储只有模拟后端、未知载荷被丢弃、必测未运行、实际模式不明、旧协议重新进入新路径，均不能写 PASS。实现与证据分开提交；停止在 P09，不进入 P10。

## 10. 可以直接交给实施方的启动指令

> P08 R1 复审通过，允许进入 P09，仅执行 P09。以 `41167a9bdff8c192fe990d53aa8dfa132d57b081` 为已验收前置，核对分支/工作区/祖先关系，保护 ProjectBuilder.cpp，不 reset、不修改 main。
>
> 读取原 P09、本启动补充及当前施工账本，按 A–F 顺序完成独立布局/恢复/偏好/目录值、完整纯验证和计划、真实存储及旧格式幂等迁移。P09 不直接修改 Root，不打开或重绑定作者内容，不提前做 P10/P12 产品应用。
>
> 使用稳定 LayoutId 文件，label 改名不改路径；计划按 ViewRestoreKey+ViewType 精确匹配，额外视图默认保留，unknown 载荷按版本和预算原样保存。请求、数据、目录和完成结果不得再混为 WorkspaceRequest。
>
> WorkspaceStore 借用 P05 同一个 WriteCoordinator 和可靠文件基础，规范化同一物理目标。写/删协调、Unknown、布局发布/偏好/目录刷新分开报告，不用直 unlink 或另一份写队列绕过在途责任。没有已验证的接口先做窄适配，不虚构后端能力。
>
> 继承 P08 R1：BUSY/权限/读取失败不是不存在，不因此清理交互、覆盖默认数据或把不完整目录报成空目录。迁移稳定映射，新记录已写/标记未写的重启不能产生重复；旧文件只读保留，新格式单向写入。
>
> 执行 X09-01～06、相关 Q、原模型/保存/Run/交互/UI 回归、真实 IO、依赖负例、SDK、八项编译负例和两项显式 GPU 模式，最终 P09 门禁。新纯验证通过不改写旧 C01 完整应用失败；C03/C04 保留原责任。
>
> 沿用 `.internal/editor-redesign/`，冻结 `dev_log/P09/`，原历史记录不改。维护精确删除/暂留清单，旧 Workspace 业务限定原消费者到 P12。实现和验收分别提交并正常推送，停在 P09 等待复审。

## 11. 固定来源

下列链接固定于本次验收 HEAD 或具体提交；编号在正文中区分源码依据与实施建议。

- [S1 分支对应验收提交](https://github.com/LUX-YU/lux-engine/commit/41167a9bdff8c192fe990d53aa8dfa132d57b081)；[实现](https://github.com/LUX-YU/lux-engine/commit/48ebdb9d8082e391f9c12619ff93934b4ee330c2)。
- [S2 P08 R1 验收报告](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/dev_log/P08-R1/README.md)。
- S3 三交互：[Scene](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/editor/tools/scene/interaction/src/SceneInteraction.cpp)、[Material](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/editor/tools/material/interaction/src/MaterialInteraction.cpp)、[Flow](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/editor/tools/flowforge/interaction/src/FlowInteraction.cpp)。
- [S4 真实三模型回归](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/editor/tests/persistence/interaction_reclaim.cpp)。
- S5 Material 实际日志：[修复前](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/dev_log/P08-R1/before/material-sync.log)、[修复后 SDK](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/dev_log/P08-R1/logs/sdk-r1-material-sync.log)。
- [S6 文件范围](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/dev_log/P08-R1/FILES.md)。
- [S7 CTest 日志](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/dev_log/P08-R1/logs/ctest.log)。
- S8 两模式：[GPU_UI](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/dev_log/P08-R1/logs/explicit-gpu_ui-ctest.log)、[EDITOR_SCENE_PANE](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/dev_log/P08-R1/logs/explicit-editor_scene_pane-ctest.log)。
- [S9 归档检查器](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/dev_log/P08-R1/check_receipt.py)。
- [S10 当前旧工作区存储](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/editor/app/src/EditorWorkspaceStorage.cpp)。
- S11 当前文件发布：[接口](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/editor/adapters/project_io/include/lux/engine/editor/io/ProjectArtifactStore.hpp)、[实现](https://github.com/LUX-YU/lux-engine/blob/41167a9bdff8c192fe990d53aa8dfa132d57b081/editor/adapters/project_io/src/ProjectArtifactStore.cpp)。
- [P09 原 V4 阶段文档](reference/v4/phases/P09_layout_storage.md)。
