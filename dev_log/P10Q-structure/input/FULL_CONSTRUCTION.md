# Lux Engine Editor 五层收敛施工总册

**2026-10-01｜P10Q-structure｜面向实施LLM**

本册是同包分批施工文件的完整合并版；不另建架构、不改P11/P12编号。路径种子须经L0核验。先按总指令执行，再按L0–L6闭合。

## 目录

1. [P10Q-structure：Editor 五层收敛施工总指令](#unit-00)
2. [裁定、继承不变量与语义归属](#unit-01)
3. [逐路径、类型与 target 处置表](#unit-02)
4. [L0：锁定输入、扩大逐文件计划、先识别混合责任](#unit-03)
5. [L1：收拢 editing 与 authoring，保持真正的纯能力闭包](#unit-04)
6. [L2：编辑活动归位，策略依赖契约，不依赖工作台](#unit-05)
7. [L3：工作台收拢，具体工具消费作者与活动，不再拥有它们](#unit-06)
8. [L4：装配边界、根构建和旧产品退场路径](#unit-07)
9. [L5：在真实复用点使用 C++20 concept，动态边界只保留一次](#unit-08)
10. [抽象、C++20 concept 与动态边界施工规格](#unit-09)
11. [构建组织与实际依赖门禁施工规格](#unit-10)
12. [验收矩阵：观察事实，不追求测试数量](#unit-11)
13. [L6：同一最终 SHA 的资格、删除检查与交付](#unit-12)
14. [P11–P13 持续约束与交接](#unit-13)
15. [施工接力、证据与最终交付格式](#unit-14)
16. [本施工包的来源、裁定和制作边界](#unit-15)

---

<a id="unit-00"></a>

<!-- 合并来源：00_MASTER.md -->

## P10Q-structure：Editor 五层收敛施工总指令

**版本：2026-10-01 / Construction 1**  
**对象：负责真实仓库实现与验收的 LLM。**  
**本文件是施工授权范围与执行规则，不是完成证明。**

### 0. 一句话任务

将已存在的新 Editor 能力，按 `editing → authoring → activities → workbench → application` 五种责任重新归属；修正真正的逆向依赖、重复协议和静态／动态绑定错误；保留已验证的行为、代码寿命和资源责任。完成后停在 P10Q 复审，不实施 P11 的新命令／扩展系统，不实施 P12 产品切换。

`A → B` 始终表示 **A 的源码或构建依赖 B 的正式契约**。标题中的五层排列是由内向外的编号顺序，**不是一条 E0 依赖 E1 的箭头链**。实际允许方向是 E4 使用 E3/E2/E1/E0，内层不得反向依赖外层；策略不包含具体后端，具体组合在实例化／装配位置完成。

### 1. 输入、优先级与范围

#### 1.1 固定参考，不强制回退

- 仓库：`LUX-YU/lux-engine`。
- 工作分支：`codex/editor-redesign-v4`。
- 本施工文档读取到的远端验收 HEAD：`f7c27f9375cbf8dd8af37b30a6027a460de26213`。
- 对应 P10Q 实现：`3c20910d05d635078ef9021a3a3318086a792b8d`。
- 实施前重新核对真实 HEAD、祖先关系和用户改动；有新提交时从真实后继继续，记录差异。**不得 reset 到本文 SHA。**
- P10Q 原报告固定的实际 lux-cxx 为 `aae62e2fc17ef78ae7be2559a6d58fa0f8297b05`。这是历史依赖事实，不是要求覆盖现有检出；记录实际正在消费的 SDK、头和依赖提交，不降级到早期审阅的版本。

#### 1.2 文件优先级

1. 用户最新明确决定：五层收敛、不要重复 engine/modules、优先做减法、Linux 当前不阻塞、不补旧慢算法样本。
2. 本施工包的明确裁定、L0–L6 任务与验收范围。
3. 附带的原五层设计及迁移约束；未被明确裁定的设计语义保持。
4. 现有生产行为和已取得的回归证据。
5. 更早的 core/services/presentation/tools 等目标目录草图，仅为历史背景，不再实施。

对照不一致时，在唯一账本写一条 `decision`，说明来源、具体冲突、如何遵守更高优先级。**禁止悄悄改设计，也禁止明知现状语义不符还照类型名搬文件。**

#### 1.3 阶段标识

正式迁移 gate 继续为 `LUX_EDITOR_MIGRATION_STAGE=P10Q`。本轮范围名固定为 `P10Q-structure`，内部批次固定为 `L0`–`L6`。在现有架构规则增加 `editor_layering` 的规则分组和严格/施工覆盖模式，不额外发明 P14、P10R 等业务阶段。

原阶段顺序 `P10 → P10Q → P11 → P12 → P13` 保持。局部 L 批次不是对 P11 的放行。

#### 1.4 本轮应完成和不应提前完成

| 必须完成 | 本轮不提前建设 |
|---|---|
| 现有正式新能力的五层归属、CMake 和安装对应 | P11 尚未实现的完整 Command/Extension 系统 |
| TaskMonitor 脱离 UI target；项目纯数据和文件副作用分开 | P12 的完整 SaveAll/Exit/OpenAndShow 产品流程 |
| 三作者模型和 CPU interaction 的独立能力闭包 | 第二个产品 exe 或新的万能 EditorApplication |
| 真实共享算法的 concept 约束、必要动态边界保留一次 | 用模板替换全部 Session/Runtime，或重写引擎框架 |
| 明确旧代码消费者、删除已到期和零消费者残留 | 为凑根目录数量把旧产品全部搬进永久 legacy |
| Windows 实际回归、SDK、实际依赖负例和受影响 GPU | 新 Linux 环境、完整 Android/macOS 矩阵、无关长基线 |

新产品入口的最终唯一切换仍由 P12 完成。此处“新链通过”指正式新模块的真实集成，不代表旧产品已经删除。

### 2. 执行方式：七个内部批次，逐个闭合

| 批次 | 文件 | 应证明的结果 |
|---|---|---|
| L0 | `phases/L0_BASELINE.md` | 当前事实、每文件去向、target 归属、用户改动和测试集合固定 |
| L1 | `phases/L1_EDITING_AUTHORING.md` | E0/E1 独立闭合；不带 UI、Process、运行实例或编译后端 |
| L2 | `phases/L2_ACTIVITIES.md` | 活动无工作台依赖；共享保存、编译、Run 与任务责任不重复 |
| L3 | `phases/L3_WORKBENCH.md` | 通用工作台与领域 UI 分开；CPU interaction 仍不依赖 ImGui |
| L4 | `phases/L4_APPLICATION.md` | 根构建、文档、测试组合明确；旧产品受控但不认证成新层 |
| L5 | `phases/L5_ABSTRACTIONS.md` | 两个真实图工具使用同一受 concept 约束的交付核；动态边界不扩张 |
| L6 | `phases/L6_QUALIFICATION.md` | 同一最终实现 SHA 上的功能、安装、依赖与删除资格 |

每批：读前置 → 更新唯一计划 → 修改一个闭合责任单元 → 定向测试 → 记录 → 提交。后批不能掩盖前批失败。

若用户把本包整体交付为一次任务，允许 L0–L6 按顺序自动执行；只在真实阻塞、计划外大范围行为改变或用户明确要求的检查点停下。若用户只点名一个 L 批次，只执行该批次。**不得以逐文件确认方式把正常实施决策反复交还用户。**

阶段门禁全量只在 L6 执行一次完整最终矩阵；前面按影响范围验证，不为每次 git mv 重跑全部长期计时。

### 3. 五层的硬边界

| 层 | 允许知道 | 不能知道 |
|---|---|---|
| E0 editing | History、Session、代际、ContentStamp、binding/checkpoint、必要纯共享身份、原基础设施 | Scene/Material/Flow 具体源、Pane、文件发布、编译或运行 |
| E1 authoring | E0；对应领域图/schema/正式纯描述/codec；项目与布局值 | TaskScope/ExecutionRuntime、SceneRuntime、ViewHost、GUI、文件后端或编译器 |
| E2 activities | E0/E1 的公开能力；原 Process/Runtime/工具链；本层窄契约 | Root/Pane/ImGui、具体 View、application 私有状态 |
| E3 通用 workbench | 原 UI/渲染/平台公开设施；必要纯身份及布局值 | 具体 SceneSession/MaterialSession/FlowSession；某个工具 UI |
| E3 领域 workbench | 对应 E1/E2 公共能力；通用 workbench | 其他工具 UI、E2 后端私有状态、application 服务定位器 |
| E4 application | 各层正式构造和调用契约；选中的具体组合 | 私下修改其他层的存储、历史和状态机 |

任何 `PRIVATE`、`LINK_ONLY`、模板实例化或生成头带来的边都算依赖。注释“只是测试”不豁免生产 target；集成测试是独立测试节点，允许组合多层，但不能把其依赖倒灌到被测库。

### 4. 开工前禁止动作

- 不改 `main`，不 force push，不 rebase 已发布历史，不执行 `git reset --hard`、`git clean -fdx`。
- 不覆盖 `ProjectBuilder.cpp` 等用户修改；未跟踪文件也不删除。先保存字节、diff 和来源，资格实现使用独立检出。
- 不新建 `EditorExecutor/EditorRuntime/EditorEventBus/EditorRenderer/ServiceLocator`。
- 不把 `Expected/Result/CodeLease/ContentStamp` 各复制一份到新目录。
- 不同时保留旧实现和新实现，再用 option 切换“待删除”；改一个完整调用链并删除原体。
- 不将缺包、缺头或缺工具造成的编译失败当作依赖/概念负例成功。
- 不靠注释掉测试、替换成 Fake、关闭生产功能或删断言获得绿色结果。
- 不将所有共享库全局改 STATIC；进程唯一身份/元信息和插件边界按现有 owner 保持。

### 5. 每个修改单元必须回答的十个问题

1. 它原来是什么事实/资源的 owner？
2. 目标层为何符合真实语义，而非名字？
3. 公开类型是否变化？变化是语义必要还是仅物理迁移？
4. 它使用了哪些实际 engine/modules/lux-cxx 能力？有没有复制？
5. 是否存在可删除的同义转发/缓存/错误翻译？
6. 采用具体类、concept、variant、virtual、signal 的理由分别是什么？
7. 旧文件、旧符号、旧 target/包的退出发生在哪个提交？
8. 哪些消费者已改？哪些旧产品消费者仍暂留？
9. 哪个行为测试证明没有回归？哪个真实负例证明禁边仍有效？
10. 哪个未测范围必须保留，不能写成 PASS？

记录采用 `templates/working-ledger.template.json` 的对应 section，或直接扩展既有同等字段；不建立两份可变事实源。

### 6. 实施验收的关键原则

**语义分层优先于移动数量。** 纯移动不是错误，但若一整批只有路径更名，没有 target 闭包、公共依赖、调用职责或文档的改善，不能报告“架构完成”。

**真实闭包优先于库数量。** 同层可以保留多个 target，跨层不应继续放在同一业务 target。公开逻辑 include 可以不变，但只有一个真实定义；不能创建 forwarding header 伪造旧路径退出。

**行为优先于原测试数字。** 输入报告的 204、178、11 等用于对账，不是新测试配额。保留实际测试名及断言/不变量映射；新增、改名、迁移都要逐项解释。

**有限测试优先于仪式化长测。** 不补旧 50k 长链的剩余 34 次；无性能算法修改时只保留短的复杂度/容量/owner 回归。

**可移植性审查不是 Linux 认证。** Windows 实测；Linux/系统 IME 没运行就保留 NOT_RUN，用户已调整范围的项目不再阻止本轮放行。

### 7. 输出与停止条件

最终交付应包括：正常实现提交、独立证据提交、准确 SHA 链、全量路径/符号/target 对照、实际依赖图、概念实例化表、删除账本、Windows/SDK/受影响 GPU 日志、Linux/IME 范围及用户改动保全证明。

冻结位置统一：`dev_log/P10Q-structure/`。日常材料继续在既有 `.internal/editor-redesign/` 的一个 `layering` 节点，不建立另一套持续变化账本。

全部必测完成才能报告本轮 PASS；未完成列明项和下一入口。**完成后停在 P10Q，不进入 P11；不得把原 P10Q PARTIAL 历史收据原地改成 PASS。**

---

<a id="unit-01"></a>

<!-- 合并来源：01_DECISIONS_AND_INVARIANTS.md -->

## 裁定、继承不变量与语义归属

本章在已认可五层设计之内细化实施；涉及对原文的纠正均显式标记，不把新推断当原资料结论。

### D01：纠正 ProjectBuilder 的旧归属推断

**源设计差异。** 原迁移表把 `ProjectBuilder` 归 activities/project，并描述其任务走 Process。此次固定提交的真实 `ProjectBuilder.hpp/.cpp` 表明：它拥有 `ProjectBuildConfig`，设置 name/plugins/initial_scene，`build() &&` 验证 Manifest、路径和 ScenePackage，然后返回配置。该代码没有调度 Process、调用工具链或写文件。[S04,S05]

**本施工裁定：** 这个现有纯配置 Builder 与 `ProjectBuildConfig` 一起放 `authoring/project`，保留准确的普通 Builder 类，不因为名字包含 Build 而移动到活动层。`ProjectCreation`、项目打开、发布、文件副作用和真正的构建执行请求才归 `activities/project`。这纠正的是原表中一个具体类型的职责判断，不改变五层设计。

**用户修改：** 该 CPP 存在已知未提交用户差异。只对固定 tracked 原体实施路径移动；工作区补丁另行保全并按新路径复现。不能凭历史哈希假定用户从未继续修改，也不能把 diff 内容混进资格提交。

### D02：ViewInfo 是纯观察值，关闭协议不是布局权威

已读 ViewInfo.hpp 同时包含纯身份／观察和 `EViewError/ViewResult/ViewCloseFailure/ViewCloseResult`。[S03]

目标分法：
- E0 的真实单一 `views/ViewInfo.hpp` 保留 `ViewId/ViewTypeId/ViewRestoreKey/ViewInfo`。它不 include Root/Pane/IViewHost；已有 `PaneTypeIdTag` 前置声明只为共享同一标签，不新造第二个 TypeId。
- 将宿主错误和关闭结果移入 E3 的一个实际 `views/ViewError.hpp`（若同义正式头已存在则合并在那里），供 IViewHost/DetachedView/具体 View 直接 include。
- authoring/layout 直接消费纯 ViewInfo 范围，不另建同字段 InventoryModel，也不持有活动目录。
- 不把 `ViewInfo` 中 visible/focused 的观察值误认为所有权；Host 仍然是唯一登记权威。

这是为避免在 E0 放具体关闭协议的施工细化。类型名称和错误枚举意义不变，消费者只改所需 include。

### D03：InteractionDelivery 不属于历史内核

当前共有 `deliverInput` 位于 `editor/editing/sinclude/.../InteractionDelivery.hpp`，实际处理 BEGIN/PREVIEW/COMMIT/CANCEL/COMPLETE 的工作台输入交付。[S07]

迁至 `editor/workbench/sinclude/lux/engine/editor/workbench/InteractionDelivery.hpp`，命名空间使用 `lux::editor::workbench::detail`；仅两个图工具 CPP/其直接测试 PRIVATE 可见。此为真实职责迁移，删除旧文件和旧 include 授权，不加转发别名，不让 widgets 认识 ContentStamp。

L5 在这一份现有算法上增加局部 concept；不新建状态机对象、不新增队列、不增加业务 target。

### D04：SourcePersistenceAccess 不按文件名一刀切

Scene 等模型中 `ScenePersistenceAccess.cpp`/私有访问可只是对 SessionState 的绑定、checkpoint、重载采用操作。若它不依赖 E2 保存类型、Process 或文件，则仍属 E1 作者模型的内部一致性机制。不要因为名称中含 Persistence 就把必须接触私有源/History 的机制硬搬 E2，再加跨层 friend/反射写口。

`SceneSaveSource/MaterialSaveSource/FlowSaveSource`、冻结编码工作与实际保存角色则归 E2。若一个源文件混用两种职责，切开“作者状态变化”与“外部保存服务接线”，不复制算法。

### D05：新层清洁与旧产品可运行同时成立

新正式 targets 必须满足 E0–E4。仍供旧产品使用的 `editor_context/editor_editing/editor_ui/旧三大Editor` 不认证成任何新内核层，而作为现有迁移账本的精确旧产品岛保留到指定阶段。

允许边：旧产品 → 新正式 API。禁止边：新正式 target → 旧产品目标或私有桥。现有正规 ProjectStorage 若仍通过 `editor_editing` 获得纯错误声明，必须提取真正需要的纯定义、移除该链接，而不是让整个 E2 白名单允许旧 editing。

新的文件不得新增旧岛消费者。已有缺陷仍保持原合同；若新链出现同类问题，另记新缺陷，不能挂 C03 延期。

### D06：CMake 单元与层是两种粒度

- History/Session 的 SHARED 身份 owner 不因合并物理路径而合成五个全局库。
- Scene/Material/Flow CPU 模型分别保留可安装 target。
- interaction 可位于 workbench，但保持 CPU target。
- TaskMonitor 与 TaskView 必須拆 target；旧包只要仍装真实 TaskView 就不算空壳。
- 带 GPU/toolchain 的集成测试移到 tests 的后配置入口，不反向增加生产库依赖。

### D07：什么必须做、什么可按证据不做

本轮必做：五层真实归属、TaskMonitor 分离、编辑/布局/UI契约拆分、旧源路径退出、输入交付 concept、目标闭包/安装/文档一致。

条件做：对冻结编码的进一步模板收敛，只有至少两个真实实现共享完整算法且不改变格式、预算、cleanup 语义时才做；否则记录保留原因。禁止为了满足概念数量配额添加未用类型。

默认不做：更换 Store 容器、修改磁盘 schema、再做组件级增量投影、全局 static 开关、Linux/Android 新矩阵、追求固定性能提升百分比。

### 继承不变量 I01–I18

| 编号 | 必须保持的事实 | 不能接受的近似替代 |
|---|---|---|
| I01 | 一个 SessionStore 独占会话；每个 Session 独立历史，共用唯一算法 | 把每个 Pane 变成内容 owner |
| I02 | current 从真实 History 读取，checkpoint/binding/admission 只有 SessionState 权威 | 新缓存 current/dirty 与原状态双向同步 |
| I03 | 一批 edits 原子提交一次；Undo/Redo 保留原身份和 payload | 分层后逐 edit 部分提交 |
| I04 | Node/Pin/变量已发号高水位与耗尽语义不回退 | 序列重建后重新从现存最大 ID 发号 |
| I05 | 冻结源深层可用；code/deleter 晚于数据析构 | 外层 shared_ptr<const T> 当全部寿命证明 |
| I06 | 回调读取及输入拒绝清理仍在正确 gate 中 | 把 cleanup 移到 scope 外 |
| I07 | BUSY/权限/IO 不等于 STALE/不存在 | 错误分支清空有效草稿或目录 |
| I08 | payload+based_on+phase 是同一被接纳输入 | 执行时重新取 current 冒充原始来源 |
| I09 | Accepted、Published/Unknown、Adopted 分开 | 一个 bool 表示保存全成功 |
| I10 | 同一个 WriteCoordinator、同目标 FIFO 与有界链证据 | 各活动私有写队列，或取消冲突检查 |
| I11 | 已准入完成可靠接收；内层完成不解除外层防重入 | BUSY 后丢唯一结果或重跑 encoder |
| I12 | Run 使用冻结输入、原 Runtime 单次驱动、停止不回写作者 | 每 View/Run 一个驱动循环 |
| I13 | 实例退休后单步结果仍可查到明确确认 | Registry 延迟不释放或直接 INVALID_ID |
| I14 | 视口本地输出/相机/高亮、共享重资产、实际 GPU 退休 | 复制整图模拟隔离 |
| I15 | 编译 key 完整、旧结果不冒充新结果、Flow 固定对象重链 | retry 时读取当前作者图 |
| I16 | ViewHost 唯一 Pane owner、Root 非拥有、离树挂载安全点 | parent/Root 和 unique_ptr 重复 delete |
| I17 | 布局纯计划不打开资产；selected 决定旧恢复来源 | 将全部 opaque locator 合并或 UI 工厂偷偷 open |
| I18 | 历史证据绑定原 SHA，当前资格绑定最终 SHA | 改旧日志路径或用早期通过覆盖最终改动 |

这些不变量来自原五层设计与 P10Q/P00–P10 回归；本包不声称已经重新执行它们。

---

<a id="unit-02"></a>

<!-- 合并来源：02_PATH_TYPE_TARGET_MAP.md -->

## 逐路径、类型与 target 处置表

以下是**施工计划种子，不是已经人工审完的逐文件删除清单**。L0 从真实 Git tree 展开到每文件并核验消费者；路径不存在则记录“已不存在”，不为满足表格重建。较长路径规则优先于较短前缀；SPLIT/REVIEW_REQUIRED 绝不自动移动。

CMake/README 多源汇入同一个领域目录时必须合并职责，不能以最后一个文件覆盖前一个。source/cmake/include/安装support要共同核对。

### 1. 路径规则

| ID | 原路径 | 目标 | 层/批次 | 动作与限制 |
|---|---|---|---|---|
| F01 | `editor/editing/history/include/` | `editor/editing/include/` | E0 / L1 | MOVE：唯一正式历史公开头 |
| F02 | `editor/editing/history/src/` | `editor/editing/src/history/` | E0 / L1 | MOVE：原算法逐文件迁入 |
| F03 | `editor/editing/history/CMakeLists.txt` | `editor/editing/CMakeLists.txt` | E0 / L1 | MERGE：保留edit_history实际目标，不创建转发target |
| F04 | `editor/editing/sessions/include/` | `editor/editing/include/` | E0 / L1 | MOVE：Session/Stamp/Permits唯一公开定义 |
| F05 | `editor/editing/sessions/src/` | `editor/editing/src/sessions/` | E0 / L1 | MOVE：Store/State/checkpoint不换算法 |
| F06 | `editor/editing/sessions/test/` | `editor/editing/test/sessions/` | TEST / L1 | MOVE_EDIT：scope_compile及SDK路径同步 |
| F07 | `editor/editing/sessions/CMakeLists.txt` | `editor/editing/CMakeLists.txt` | E0 / L1 | MERGE：原session目标和共享状态保持 |
| F08 | `editor/contracts/include/lux/engine/editor/views/ViewInfo.hpp` | `editor/editing/include/lux/engine/editor/views/ViewInfo.hpp` | E0+E3 / L1 | SPLIT：纯观察留E0；UI错误到workbench/desktop/include/.../views/ViewError.hpp |
| F09 | `editor/contracts/` | `editor/editing/` | REVIEW / L1 | REVIEW_REQUIRED：必须逐文件排除UI-only；不整目录覆盖 |
| F10 | `editor/editing/sinclude/lux/engine/editor/editing/InteractionDelivery.hpp` | `editor/workbench/sinclude/lux/engine/editor/workbench/InteractionDelivery.hpp` | E3 / L3+L5 | MOVE_EDIT：namespace改workbench::detail；双工具PRIVATE消费 |
| F11 | `editor/editing/include/lux/engine/editor/EditorError.hpp` | `editor/editing/include/lux/engine/editor/EditorError.hpp` | E0 / L1 | KEEP_RECLASSIFY：纯错误定义真实provider，不要求consumer链接旧editor_editing |
| F12 | `editor/editing/scene/` | 按符号裁定／暂留原位 | TEMPORARY / L0 | REVIEW_REQUIRED：旧Registry适配按消费者到P12；不可认证E0 |
| F13 | `editor/editing/include/` | 按符号裁定／暂留原位 | REVIEW / L0 | REVIEW_REQUIRED：除已列纯头外识别旧AssetSave/Close/EditHistoryTarget |
| F14 | `editor/editing/src/` | 按符号裁定／暂留原位 | REVIEW / L0 | REVIEW_REQUIRED：原EditHistoryTarget及历史桥不混进E0 |
| F15 | `editor/editing/sinclude/` | 按符号裁定／暂留原位 | REVIEW / L0 | REVIEW_REQUIRED：SignalDelivery/TaskResult按实际consumer归属，不默认新common |
| F16 | `editor/tools/scene/model/` | `editor/authoring/scene/` | E1 / L1 | MOVE_EDIT：PersistenceAccess按作者状态语义保留 |
| F17 | `editor/tools/material/model/` | `editor/authoring/material/` | E1 / L1 | MOVE_EDIT：原MaterialSource/图/快照不重写 |
| F18 | `editor/tools/flowforge/model/` | `editor/authoring/flow/` | E1 / L1 | MOVE_EDIT：物理flow；逻辑flowforge/schema保留 |
| F19 | `editor/project/ui/` | `editor/workbench/project/` | E3 / L3 | MOVE_EDIT：纯UI；CatalogModel不跟随 |
| F20 | `editor/project/src/ProjectBuilder.cpp` | `editor/authoring/project/src/ProjectBuilder.cpp` | E1 / L1 | PROTECTED_MOVE：D01纯Builder；用户未提交bytes单独保护 |
| F21 | `editor/project/` | `editor/authoring/project/` | E1 / L1 | MOVE_EDIT：仅真实纯Manifest/Builder/Catalog；混合副作用另分 |
| F22 | `editor/workspace/layout/` | `editor/authoring/layout/` | E1 / L1 | MOVE_EDIT：纯Layout/Recovery/Preferences/plan |
| F23 | `editor/persistence/` | `editor/activities/persistence/` | E2 / L2 | MOVE_EDIT：core/execution继续实际两个边界 |
| F24 | `editor/storage/src/FileArtifactStore.cpp` | `editor/activities/persistence/src/FileArtifactStore.cpp` | E2 / L2 | MOVE_EDIT：真实文件后端不叫compatibility |
| F25 | `editor/storage/src/FilePublication.cpp` | `editor/activities/persistence/src/FilePublication.cpp` | E2 / L2 | MOVE_EDIT：唯一平台文件发布算法 |
| F26 | `editor/storage/include/lux/engine/editor/storage/FileArtifactStore.hpp` | `editor/activities/persistence/include/lux/engine/editor/storage/FileArtifactStore.hpp` | E2 / L2 | MOVE_EDIT：逻辑include可保留 |
| F27 | `editor/storage/include/lux/engine/editor/storage/FilePublication.hpp` | `editor/activities/persistence/include/lux/engine/editor/storage/FilePublication.hpp` | E2 / L2 | MOVE_EDIT：实际后端契约唯一 |
| F28 | `editor/storage/` | `editor/activities/project/` | E2 / L2 | REVIEW_REQUIRED：逐文件；ProjectStorage/Creation/Publication，CMake拆两主题 |
| F29 | `editor/assets/` | `editor/activities/project/` | REVIEW / L2 | REVIEW_REQUIRED：源读取导入归E2；旧TAssetSave不可整包迁入 |
| F30 | `editor/tools/scene/persistence/` | `editor/activities/scene/` | E2 / L2 | MOVE_EDIT：实际角色target保持，不并入通用保存core |
| F31 | `editor/tools/material/persistence/` | `editor/activities/material/` | E2 / L2 | MOVE_EDIT：具体源格式及采用 |
| F32 | `editor/tools/flowforge/persistence/` | `editor/activities/flow/` | E2 / L2 | MOVE_EDIT：固定源编码/采用 |
| F33 | `editor/tools/scene/execution/api/include/` | `editor/activities/scene/include/` | E2 / L2 | MOVE_EDIT：纯RunInspect/API实际闭包保持 |
| F34 | `editor/tools/scene/execution/` | `editor/activities/scene/` | E2 / L2 | MOVE_EDIT：RunStore与Controller原实现 |
| F35 | `editor/tools/scene/projection/test/` | `editor/tests/integration/projection/` | TEST / L2 | MOVE_EDIT：组合viewport为测试依赖；生产不倒灌 |
| F36 | `editor/tools/scene/projection/` | `editor/activities/scene/` | E2 / L2 | MOVE_EDIT：SceneProjection/Hub/ResourceStatus；多个CMake要合并审查 |
| F37 | `editor/tools/material/preview/test/` | `editor/tests/integration/material_activity/` | TEST / L2 | MOVE_EDIT：编译/预览真实fixture整合后配置 |
| F38 | `editor/tools/material/preview/` | `editor/activities/material/` | E2 / L2 | MOVE_EDIT：compile/preview保留不同真实闭包 |
| F39 | `editor/tools/flowforge/compilation/` | `editor/activities/flow/` | E2 / L2 | MOVE_EDIT：固定编译对象重试 |
| F40 | `editor/workspace/storage/` | `editor/activities/workspace/` | E2 / L2 | MOVE_EDIT：只文件/迁移，不调用ViewHost |
| F41 | `editor/tasks/ui/include/lux/engine/editor/tasks/TaskMonitor.hpp` | `editor/activities/tasks/include/lux/engine/editor/tasks/TaskMonitor.hpp` | E2 / L2 | MOVE_EDIT：新增真实CPU target editor_tasks |
| F42 | `editor/tasks/ui/src/TaskMonitor.cpp` | `editor/activities/tasks/src/TaskMonitor.cpp` | E2 / L2 | MOVE_EDIT：不再编进tasks_ui |
| F43 | `editor/tasks/ui/test/monitor.cpp` | `editor/activities/tasks/test/monitor.cpp` | TEST / L2 | SPLIT：纯observer测试留E2；含View的段移integration |
| F44 | `editor/tasks/ui/` | `editor/workbench/tasks/` | E3 / L3 | MOVE_EDIT：剩余TaskView；包仍是真UI而非旧alias |
| F45 | `editor/views/api/include/` | `editor/workbench/desktop/include/` | E3 / L3 | MOVE_EDIT：View契约与Host同主题；view_api目标保留 |
| F46 | `editor/views/api/` | `editor/workbench/desktop/` | E3 / L3 | MOVE_EDIT：CMake和README需合并非覆盖 |
| F47 | `editor/views/viewport/` | `editor/workbench/viewport/` | E3 / L3 | MOVE_EDIT：ViewportElement等不带Scene作者 |
| F48 | `editor/widgets/` | `editor/workbench/widgets/` | E3 / L3 | MOVE_EDIT：原node-editor backend保留 |
| F49 | `editor/desktop/` | `editor/workbench/desktop/` | E3 / L3 | MOVE_EDIT：Host/Shell唯一实现 |
| F50 | `editor/tools/scene/interaction/` | `editor/workbench/scene/interaction/` | E3-CPU / L3 | MOVE_EDIT：CPU target不能加GUI |
| F51 | `editor/tools/material/interaction/` | `editor/workbench/material/interaction/` | E3-CPU / L3 | MOVE_EDIT：CPU target保持 |
| F52 | `editor/tools/flowforge/interaction/` | `editor/workbench/flow/interaction/` | E3-CPU / L3 | MOVE_EDIT：完整ContentStamp选择来源保持 |
| F53 | `editor/tools/scene/ui/test/` | `editor/tests/integration/scene_views/` | TEST / L3 | MOVE_EDIT：跨工具/桌面/SDK fixture最后配置 |
| F54 | `editor/tools/scene/ui/` | `editor/workbench/scene/` | E3 / L3 | MOVE_EDIT：代码生成器/support同步 |
| F55 | `editor/tools/material/ui/` | `editor/workbench/material/` | E3 / L3 | MOVE_EDIT：草稿/queue原语义保持 |
| F56 | `editor/tools/flowforge/ui/` | `editor/workbench/flow/` | E3 / L3 | MOVE_EDIT：逻辑namespace/include不机械改名 |
| F57 | `editor/tools/settings/` | `editor/workbench/settings/` | REVIEW / L4 | REVIEW_REQUIRED：只迁无Context的真实UI；旧接线暂留 |
| F58 | `editor/metadata/` | 按符号裁定／暂留原位 | REVIEW / L4 | REVIEW_REQUIRED：纯值/工厂/注册/装载四分，旧P11登记暂留 |
| F59 | `editor/plugins/` | 按符号裁定／暂留原位 | REVIEW / L4 | REVIEW_REQUIRED：贡献归相应层，安装归application/extensions |
| F60 | `editor/app/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：已可分离装配归application；旧EditorImpl到P12 |
| F61 | `editor/launcher/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：真实新launch可迁；旧产品流程到P12 |
| F62 | `editor/context/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：不改名成新的Context |
| F63 | `editor/ui/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：旧产品UI，未引用原体删除；不搬永久legacy |
| F64 | `editor/transition/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：按消费者删除，禁止新增 |
| F65 | `editor/tools/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：剩余三大旧Editor/旧项目UI，只旧consumer |
| F66 | `editor/tests/` | `editor/tests/` | TEST / L6 | KEEP_EDIT：归属/路径/negative fixtures更新；历史日志不改 |
| F67 | `editor/CMakeLists.txt` | `editor/CMakeLists.txt` | BUILD / L4 | REWRITE：新五层配置+显式暂留旧岛 |
| F68 | `editor/README.md` | `editor/README.md` | DOC / L4 | REWRITE：现状/目标/旧产品区分 |
| F69 | `editor/editing/CMakeLists.txt` | `editor/editing/CMakeLists.txt` | BUILD / L1 | REWRITE：E0实际目标与暂留旧目标明确区分 |
| F70 | `editor/editing/README.md` | `editor/editing/README.md` | DOC / L1 | REWRITE：新语义，旧业务不是正式内核 |
| F71 | `editor/` | 按符号裁定／暂留原位 | REVIEW / L0 | REVIEW_REQUIRED：所有未匹配tracked文件必须人工裁定，不忽略 |

### 2. 类型／成员动作

| 类别 | 动作 | 必须删除或维持的内容 |
|---|---|---|
| EditHistory/SessionStore/SessionState | KEEP_MOVE | 算法与 authority 不换；不新增 façade、current 或 dirty |
| IEditSession | KEEP_DYNAMIC | 保留异构描述/关闭；不为了concept把Store模板化 |
| CodeLease/Permits/ContentStamp | KEEP | 数据与code析构顺序、域/代际不可删 |
| ViewInfo | SPLIT_HEADER | 纯观察仍唯一定义；UI错误出E0；不另造同步目录 |
| ProjectBuilder/ProjectBuildConfig | KEEP_AUTHORING | D01纠正旧表；纯配置验证不改成执行服务 |
| ProjectCatalogModel/Snapshot | KEEP_AUTHORING | 一份Data/数组/索引/revision，Storage只组合 |
| ProjectStorage | MOVE_ACTIVITIES | 文件/目录采用责任；解除仅为纯头而链接旧editing |
| TaskMonitor | SPLIT_TARGET | E2应用观察服务；从tasks_ui source列表彻底删除 |
| TaskView | MOVE_WORKBENCH | 借用Monitor；不直接抢observer |
| InputDelivery stage/helper | MOVE_CONSTRAIN | 工作台私有共享；旧editing路径删除 |
| CanvasRequest/NodePropertiesDraft | KEEP | payload+based_on+phase同一输入，不执行时补current |
| ISaveSource/IEncodeJob/IPreparedRebind | KEEP_DYNAMIC | 一个实际开放边界；不嵌套多层新wrapper |
| MaterialCompileOperation/FlowCompileOperation | KEEP_OWNER | 禁复制/按值移动；结果仍可共享 |
| Publish*Operation旧静态壳 | MUST_STAY_DELETED | 已在P10Q退出，不重建；自由函数使用唯一发布准入 |
| 三个公共SessionAccess别名头 | MUST_STAY_DELETED | 不重建；真正detail访问实现不误删 |
| DetachedView/Mount准备 | KEEP_MOVE_ONLY | 移动赋值/销毁均保持code晚于节点 |
| Runtime/RunStore/PreviewStore | KEEP_AUTHORITY | 不因分层重复记录实际资源/结果 |
| 旧Context/Editor/注册体 | RETAIN_LIMITED | 精确consumer直到P11/P12；不是新的authoring/application |

### 3. target 处置计划

下表中的“建议新增”是本施工决策；没有标为建议的新名字不得被当作已存在target。实际 target 名从 L0 解析，不根据路径猜。

| 现有边界 | 目标层 | 默认二进制决定 | 改动 |
|---|---|---|---|
| editor_contracts / edit_history / edit_sessions | E0 | 保留当前已资格的共享状态边界 | 合并物理include/src；多target仍可同目录 |
| scene_model / material_model / flowforge_model | E1 | STATIC | 源路径迁入authoring，不引入toolchain/UI |
| editor_project | E1 | 原STATIC保持 | 纯Manifest/Model/Builder，不加文件执行 |
| 原纯layout target | E1 | 原形式保持 | 只纯值/计划，ViewInfo provider改后续实际定义 |
| editor_persistence | E2 | STATIC | 不include具体Session/后端 |
| editor_persistence_execution | E2 | STATIC | TaskScope接线，与core主题相同、closure不同 |
| 原FileArtifactStore/FilePublication targets | E2 | 原STATIC保持 | 迁入persistence主题，安装不借旧storage头 |
| 原三类persistence targets | E2相应域 | STATIC | 实际格式/采用角色，不形成新port层 |
| scene_execution_api / scene_execution | E2 | 按现有格式 | CPU inspect边界保持，render_client已声明依赖不能丢 |
| scene_projection | E2 | STATIC | viewport仅测试依赖，不进生产目标 |
| material编译/预览、flowforge_compilation | E2 | 按实际CPU/toolchain/GPU边界保留 | 不为同域对称合成巨型服务 |
| editor_tasks（建议新增真实target） | E2 | STATIC | 只编TaskMonitor，证明无GUI安装消费 |
| tasks_ui | E3 | STATIC | 只编TaskView，消费editor_tasks |
| view_api / view_host / desktop_shell / editor_viewport | E3 | 沿现有真实边界 | 公共契约/host/图形独立，不一库打包 |
| scene/material/flow interaction | E3-CPU | STATIC | 路径在workbench不等于依赖ImGui |
| scene_ui/material_ui/flow_ui/project_ui/widgets | E3 | STATIC | 同域公开API＋通用设施，不互相带入工具UI |
| 新组合入口/launcher | E4 | exe或真实内部STATIC | 不安装第二测试产品，不构造万能Context |
| 旧metadata/context/editor_ui/旧三大Editor | 临时旧产品 | 本轮保留已必要形式 | 精确登记，不换名认证新层；零消费者即删除 |

target 与安装包保持原名时只需 source/provider 改变，不算“兼容壳”。反之，留下一个旧 target 只 `INTERFACE_LINK_LIBRARIES` 转发到新 target 来保持旧名，就是本轮禁止的空壳。

### 4. 同时必须更新的非 Editor 路径

- `cmake/EditorTests.cmake`：读取layering规则/测试能力/阶段顺序的实际路径。
- `cmake/EditorArchitectureChecks.cmake` 与 `editor/tests/architecture`：源、头、target角色、实际依赖负例和历史SHA规则。
- `cmake/installed-consumers/**`：真实代码源路径、find_package、模式、运行库搜索与测试名映射。
- 代码生成器、meta/IR输入、support install、导出配置；相对路径以新目录为准。
- 根 `AGENTS.md` 和正式架构说明：新层/旧产品区分，禁止把参考演示代码复制进生产。
- 外部仓库只有在真实消费闭包需要变更时才改；不以本轮目录迁移为由修改所有engine/modules接口。

`dev_log/P00..P10Q`、原规格包、原始before负例和用户数据文件不批量改路径。当前计划/测试使用新路径，历史验证使用旧实现SHA。

---

<a id="unit-03"></a>

<!-- 合并来源：phases/L0_BASELINE.md -->

## L0：锁定输入、扩大逐文件计划、先识别混合责任

**本批次不改变生产行为。完成后应有可执行计划，不是泛泛架构感想。**  
前置阅读：00_MASTER、01_DECISIONS、02_PATH_TYPE_TARGET_MAP、04_BUILD_AND_DEPENDENCY。

### 1. 建立唯一施工位置

继续使用 `.internal/editor-redesign/`。已有 JSON 账本能够容纳 layering 时直接追加节点；若过大，可在同目录建立一个 `layering/` 分片，但主账本只引用该分片，不复制同一文件/target/测试状态。

最少数据：

| 分片 | 必备内容 |
|---|---|
| baseline | input SHA、实际工作 HEAD、分支、依赖 SHA/安装路径/构建工具、用户改动原字节 |
| decisions | D01–D07 及动工发现的真实差异、处理理由 |
| file_plan | 每个 tracked Editor 文件、原 blob、目标层/路径、动作、调用者、批次、验收点 |
| targets | 每个生产/测试/生成/安装 target 的角色、层、来源、依赖、STATIC/SHARED 理由 |
| symbols | 拆分/合并/删除的类型、函数、成员及消费者 |
| verification | 继承测试集合、按批次增量结果、最终同 SHA 资格 |
| handoff | 当前完成批次、下一个允许入口、真实阻塞、用户改动位置 |

允许合并为一个账本文件；不要求制造七份独立文档。

### 2. 工作区与参考版本核对

按以下顺序操作，命令结果存入账本引用的日志，而不是只复制终端截图：

```text
git status --porcelain=v1 -z
git rev-parse HEAD
git branch --show-current
git remote -v
git log -1 --format=fuller
git diff --binary
git diff --cached --binary
```

核对 HEAD 与参考验收 SHA 的祖先关系。分支有新的合法提交时，差异归入本轮输入，并说明已完成哪些目标；不反复实施或恢复旧文件。

`ProjectBuilder.cpp` 保护流程：
1. 读取实际 bytes/hash 与 tracked blob 分开保存。
2. 导出未提交 binary diff，保存同目录未跟踪关联文件清单。
3. 不执行 `git add .`，不对带用户差异的原工作区直接批量 git mv。
4. 首选独立 detached worktree 或 clone 完成 tracked 迁移和资格，原工作区完全不动。若需推送实施分支，普通 fast-forward push 当前实现 HEAD 到指定远端分支；有并发新提交先处理，绝不 force。
5. 交付记录用户补丁对应的新逻辑路径；未经授权不替用户提交该补丁。报告区别“实施检出干净”与“原工作区仍有用户修改”。
6. 用户修改后的代码若与新头路径不兼容，只提供单独重定位补丁/冲突说明，不能悄悄把它改写成阶段实现。

### 3. 从真实 Git 清单生成计划

可使用本包 `scripts/plan_inventory.py`：它仅运行只读 git 命令并写一个独立 JSON 草案，不移动、不删除、不 stage。示例：

```text
python scripts/plan_inventory.py --repo <仓库路径> --ref HEAD --rules templates/path-rules.json --out <唯一施工目录>/inventory.proposed.json
```

规则是前缀级**计划种子**，不是已人工审核的全仓清单。脚本未识别的文件必须标 REVIEW_REQUIRED；禁止默认丢弃。CMake/README/生成器/混合目录默认需要人工核验。

将草案并入唯一 `file_plan`，逐项补齐：
- 是否为生产、测试、生成输入、安装支持或历史/旧产品文件；
- 当前实际 target（从 CMake File API 与 compile_commands，不靠文件名猜）；
- 原物理路径、逻辑 include、原 blob；
- 目标层、目标路径、目标 target；
- MOVE / SPLIT / MERGE / DELETE / KEEP / TEMPORARY_P11 / TEMPORARY_P12；
- 本轮删除条件与保留理由；
- 真实 include/链接/生成/SDK 消费者；
- 负责 L 批次和验收条目。

**一对多 SPLIT 必须细到符号或行段职责。** 如 ViewInfo.hpp 分出 ViewError.hpp，不能只写“整理 contracts”。

### 4. 盘点必须覆盖的高风险混合点

| 混合点 | 必须分清的事实 |
|---|---|
| editing 根与 history/sessions | 原 LegacyPersistenceState/EditHistoryTarget 与新 E0 不是一套责任 |
| contracts/ViewInfo.hpp | 纯观察值与 UI 关闭错误的拆分，保留唯一身份 |
| project/ProjectBuilder | 实际为纯配置 Builder，不能按上一稿错误描述归异步层 |
| storage | 通用文件发布与项目目录/项目事务；旧 error 声明不是旧框架链接理由 |
| tools/*/model 的 PersistenceAccess | 作者自身状态操作与 E2 保存角色分开 |
| tasks/ui | Monitor 不需要 UI；View 是消费者 |
| scene/projection 的 test | 集成测试链接 viewport 是合法测试组合，不是生产逆向边 |
| metadata/plugins | UI工厂、纯值、运行装载、贡献安装按语义分类；旧登记暂留不能变基础 |
| views/api 与 layout | 纯布局计划不能借活动 Root，不能靠挪 Interface 目录遮掩 |
| editing/sinclude | InteractionDelivery 归工作台；SignalDelivery/TaskResult 按真实消费者逐个审查 |

### 5. 记录实际 build/安装与依赖

读取 P10Q 收据中的实际配置命令，在独立 build tree 导出当前 CMake File API codemodel/target 及编译数据库。记录原：
- CTest name/labels/command/可用环境；
- 生产能力与测试能力的开关，不能混淆；
- 安装包、export target、公共头和生成头；
- 原 14 组 SDK 及额外质量消费者的真实测试映射；
- 显式 GPU 模式和 native 输入命令；
- 需要保留的进程共享状态和 SHARED 边界。

无需为 L0 重新运行全部旧版 50k 长测。已有 baseline 的逐字日志是事实；用 `ctest --show-only=json-v1` 建立名称清单即可，只有基线构建已坏或不可信时才先作必要恢复。

### 6. 计划内纠正与未知边界

D01 已用真实源码核实，可直接采用。其他分类若与当前代码不符，记录具体文件和调用，先改计划再改代码。

不能把没有读取的文件写成“可安全删除”；不能因为本包路径种子缺一个文件而排除它。发现源文件已在新位置则复用，不再复制回预设路径。

### 7. L0 验收与交付

- 每个 tracked Editor 文件恰有一条去向或明确 split；非 Editor 的受影响 CMake/SDK/脚本也在计划中。
- 所有公开定义的唯一拥有路径可确定；没有多个文件计划生成同名逻辑 include。
- 原新/旧 target 角色标记准确；可选环境与结果范围写清。
- 输入设计的 D01 纠正和 Linux/性能范围调整已新增记录，旧收据字节未变。
- 用户补丁已保全，原工作区没有被清理。

提交仅含本轮施工元数据/必要规则骨架；若 `.internal` 原本不提交，则正常保留本地，最终冻结到 dev_log。记录 `L0 complete` 和 L1 的具体入口，不声明任何新行为通过。

---

<a id="unit-04"></a>

<!-- 合并来源：phases/L1_EDITING_AUTHORING.md -->

## L1：收拢 editing 与 authoring，保持真正的纯能力闭包

目标：E0/E1 的文件、target、公开定义和依赖一致；没有因路径重组复制 History、Source、codec 或项目目录。先读 L0 计划和所有实际 target。

### 1. 推荐提交单元

L1a：E0 纯身份/历史/会话物理归并及 CMake。  
L1b：ViewInfo 与 ViewError 分开；所有即时消费者一次改齐。  
L1c：三个作者模型和布局迁移。  
L1d：项目纯 Manifest/目录/Builder 迁移、与文件活动的依赖切开。

每单元可构建再提交；不能提交一个默认产品无法配置的半移动树。暂留旧产品的配置在独立明确段保持，不把新 target 链到旧聚合包。

### 2. E0 文件归并

| 原位置 | 目标位置 | 具体处理 |
|---|---|---|
| `editor/editing/history/include/**` | `editor/editing/include/**` | 按原逻辑 include 合并；不得覆盖同名旧头 |
| `editor/editing/history/src/**` | `editor/editing/src/history/**` | 保留历史算法原体，不新建 HistoryFacade |
| `editor/editing/sessions/include/**` | `editor/editing/include/**` | Session/Stamp/Permits 唯一定义 |
| `editor/editing/sessions/src/**` | `editor/editing/src/sessions/**` | Store、State、checkpoint 原实现 |
| `editor/editing/sessions/test/**` | `editor/editing/test/sessions/**` | 修改 scope_compile 脚本路径和 include 输入 |
| `editor/contracts` 中共同值 | `editor/editing/include/...` / 相应 src | CodeLease 与纯身份保持同一类型和二进制 owner |
| history/sessions/contracts 子 CMake | `editor/editing/CMakeLists.txt` | 同一目录可定义原多个真实 target；删除已空子 CMake |

如果原 contracts 包含非纯生命周期/视图实现，不按默认归入 E0。每个符号按 D02/路径表判断。

#### 必须保留的类型/函数契约

- `IEditSession` 的描述与私有关闭/内容戳查询；不向公共接口暴露 currentContent 以节省查找。
- `SessionStore` 的 reserve/prepare/publish、Store 域/slot/generation、typed access、reclaiming/dispatch 保护。
- `SessionState` 的 binding/checkpoint/gate 与原 withRead/withEdit 语义。
- `EditHistory` 的唯一算法及候选保留、NO_CHANGE、Undo/Redo、memento/code 顺序。
- `CodeLease` 现有代码 owner；不能用空 builtin lease 替换插件输入来简化迁移。

#### 旧 editing 不混入新 E0

原 `AssetSave/CloseRequest/AssetEditing/EditHistoryTarget/LegacyPersistenceState` 等含旧产品责任的源码仍在原位置或按实际消费者退出。不得把它们编进 `edit_history/edit_sessions/editor_contracts`。

现有 `EditorError.hpp` 是纯错误值（含 std::any cause），可按真实跨模块需要迁入 E0 唯一轻量声明；这不是要求所有 Result 改用它。旧 header 的消费者直指同一真实定义，不能仅为一个声明继续链接 `editor_editing`。外层字符串/any 错误的代码寿命维持原契约，不在本轮顺手泛化错误框架。

### 3. ViewInfo / ViewError 拆分

1. 原 `views/ViewInfo.hpp` 中保留 `ViewId/ViewTypeId/ViewRestoreKey/ViewInfo`，移至新 E0 include 根。
2. 将 `EViewError/ViewResult/ViewCloseFailure/ViewCloseResult` 移入 `editor/workbench/desktop/include/lux/engine/editor/views/ViewError.hpp`。
3. 后者直接 include 前者；IViewHost、DetachedView、具体 Views 直接 include 后者。E0 不反 include。
4. authoring/layout 不依赖后者。若某段纯规划目前返回 EViewError，改用已有布局域错误，不通过 Workbench Result 作为全局错误。
5. 保留已有枚举值和对外行为；变化局限于契约归属。需要改调用者时在同提交改齐。
6. 布局观察继续用现有纯 ViewInfo，不新增第二套相同字段目录；Host 返回的是一次观察，不额外维护一个 Model。

此处新 E3 头可在 L1 创建，代表提前建立真实契约依赖，不等于提前做整个 L3。CMake 只给它一个已有 view_api 的明确头来源，不增加 ViewError 专用库。

### 4. 三个作者模型

整包保留内部 `include/src/edits/test` 组织，迁至：

```text
editor/authoring/scene/       ← tools/scene/model
editor/authoring/material/    ← tools/material/model
editor/authoring/flow/        ← tools/flowforge/model
```

公开 `lux::editor::flowforge` 与 include 中 flowforge 保持；物理短名 flow 不触发 schema/namespace/资产格式批量重命名。

处理步骤：
1. 沿 L0 每文件清单移动；测试/生成输入/README/CMake 同步。
2. 保留 `scene_model/material_model/flowforge_model` 名称（若 L0 实际名称不同，以实际为准，记录而非偷偷另造）。
3. PRIVATE 依赖仍是实际依赖，核对公开头中需要的 graph/schema/codec 已有 target。
4. `ScenePersistenceAccess.cpp` 等先按 D04 分类。作者本地绑定/基线操作不强行搬出。
5. 纯保存角色从本包外部迁 E2，不让 E1 include SaveService 或 Process。
6. 旧工具继续调用迁出的同一纯算法，改真实 CMake 引用；不增加 forward header。

### 5. authoring/project：模型与实际项目执行分离

正式迁入：ProjectManifest 及验证/codec、AssetCatalog 值、ProjectCatalogModel/Snapshot、ProjectBuildConfig、现有纯 ProjectBuilder。

明确不迁入：ProjectStorage 的磁盘/VFS/TaskScope、ProjectCreation 文件创建、ProjectPublication、ProjectAssetSource 的 IO 工作、真正的 toolchain 构建操作。

`editor_project` 原混合 CPP 在目标结构中如果全部属于上述纯数据/Builder，可保留这个实际 STATIC target。不要为配合旧设计错误再拆出一个空 `project_builder_execution`。

目录数据保留一个 owner：Model 内不可变 Data 包含数组和索引，Storage 组合 Model，UI 借用 Model/Snapshot。Model 的 LuxObject 变化通知是允许的基础依赖，不代表它依赖 UI。

Builder 的受保护 CPP 在独立 tracked 检出迁移；不得把用户差异作为本阶段功能。

### 6. authoring/layout

将 workspace/layout 的真实纯值/计划/验证移至 authoring/layout。需要 `layout/recovery/preferences` 主题时放同一领域内，不新建三个 ModelManager。

保持：稳定 LayoutId、ViewRestoreKey+ViewType 精确匹配、额外视图保留、预算/树验证、合法未知载荷、纯计划不打开资产。

计划的 inventory 取自调用者传入的纯观察值。不要把读取 Root 的代码移进这个层；真正执行计划由工作台和 application 组合。

### 7. CMake 与独立消费

- `editor/authoring/CMakeLists.txt` 只配置五个实际领域。
- authoring 生产 target 不得链接 Process、SceneRuntime/composition、render runtime、material compiler、flow linker、workbench 或 legacy target。
- Scene 使用既有纯 SceneAsset/Camera/schema/World materialization 不等于运行实例；按实际闭包区分，不能只按 `scene` 字符串全禁。
- 应在最小消费工程中只 find/install 对应模型，不需要配置工作台或真正 compiler 的开发包；若现有安装 find 递归引入无关包，修正依赖声明，不能仅关闭测试。
- 三种模型测试仍使用真实源、历史和 codec，不替换成 FakeSession。

### 8. L1 出口

必须通过 XL01–XL06 的对应观察：三模型 CPU 正例、禁止外层依赖负例、唯一 E0 定义、纯布局、项目目录和 Builder 行为、旧产品依然可配置。

检查更名范围：不得出现新的 SceneSource/History/MaterialGraph 实现副本、旧 alias wrapper、旧头和新头同时安装、或者 schema/数据格式变化。

交付列出所有旧文件、目标位置、删除的空 CMake、原 target 的实际 source 列表、尚存旧 editing 的精确消费者。不要以“根下还有旧 editing 头”否定已分离的 E0，也不要将它们当永久 E0。

---

<a id="unit-05"></a>

<!-- 合并来源：phases/L2_ACTIVITIES.md -->

## L2：编辑活动归位，策略依赖契约，不依赖工作台

前置：L1 新 E0/E1 闭包通过；已有 E2 行为不重写。该批最重要的不是目录，而是 TaskMonitor 的 target 分离、保存策略独立、项目副作用与纯数据分开，以及旧依赖退出。

### 1. 活动包迁移表

| 目标包 | 正式迁入 | 本包不拥有 |
|---|---|---|
| persistence | SaveService、WriteCoordinator、SaveExecution、EncodeJob、FileArtifactStore/FilePublication | 三种作者源、Pane、运行实例、第二 Executor |
| scene | SceneSaveSource/源编码角色、RunStore/RunController、SceneProjection/Hub/ResourceStatus | viewport 相机输出、第二 Runtime 驱动 |
| material | 保存角色、MaterialCompilationService/Operation、PreviewStore、发布函数 | MaterialSource 权威、MaterialView |
| flow | 保存角色、FlowCompilationService/Operation、固定对象/链接尝试、发布函数 | 可写 FlowGraph、FlowView |
| project | ProjectStorage、实际 ProjectCreation/Publication/导入/VFS接线 | 第二 ProjectCatalog 数组/版本 |
| workspace | WorkspaceStore/LegacyWorkspaceImporter/文件迁移操作 | DockLayout 权威副本、ViewHost |
| tasks | TaskMonitor | 任务调度/执行、TaskView |
| commands | 不创建空 target | P11 才建设真正命令系统 |

`activities` 目录不要求类都改名 Activity；已有正确类型名保持。

### 2. 先处理 TaskMonitor，证明无 UI 消费

#### 文件与 target

- 从 `editor/tasks/ui/include/.../TaskMonitor.hpp` 移到 `editor/activities/tasks/include/.../TaskMonitor.hpp`。
- 从 `editor/tasks/ui/src/TaskMonitor.cpp` 移到同包 src。
- 新增一个真实 CPU STATIC target，建议名 `editor_tasks`；仅链接实际 LuxObject、Process 的正式 API 和必要轻量依赖。
- `tasks_ui` 删除 Monitor.cpp，转而 PUBLIC/PRIVATE 使用新 target，按实际 header 是否暴露 Monitor 类型决定。
- 旧 `tasks-ui` 包若仍装 TaskView 是真实包，可保留；新 `editor-tasks` 安装组件暴露 Monitor，不含 view_api/ImGui。
- 更新旧 Context/TaskPane 和新 TaskView 的构造，仍借用同一个应用级 Monitor，不能各自占用 Runtime observer。

#### 不允许改变

Runtime 的 taskInfos/TaskInfo 是唯一任务事实，Monitor 的 snapshot 是有版本的只读投影；不存可写任务状态机。observer 回调只标记待通知，真正通知在原 dispatch 退出后。

#### 回归

两个真实 Monitor 消费者共享一次观察事实；多个 TaskView 关闭不取消外部 owner 的任务；Runtime stop/finish 仍可结清；CPU consumer 不 require Pane/ImGui/ViewHost。验证链接命令和 include 依赖，而非仅不创建 UI。

### 3. persistence：同主题内保留必要的核心与执行边界

建议保留原 `editor_persistence` 与 `editor_persistence_execution` 两个 STATIC target。

纯 core 源：SaveService/WriteCoordinator/EncodedArtifact/发布协议；不包含具体 Session、FileArtifactStore 或 ExecutionRuntime。

执行与后端：SaveExecution 使用原 TaskScope；FileArtifactStore/FilePublication 放同一活动主题的私有实现组，可以保留原真实后端 target，不为一个 CPP 新增空接口。

#### 具体动作

1. 将 `editor/persistence` 搬到 `activities/persistence`；文件后端从原 storage 迁入同一主题，不复制 FilePublication 算法。
2. 保留逻辑 include 中 persistence/storage 的既有准确名字。仅物理移动时无需再发明 `/activities/` 前缀。
3. `ISaveSource`、IEncodeJob、IPreparedRebind 保持必要动态契约；本轮不再套 Provider/Adapter/Port。
4. 三类保存实现各进对应活动包，通用核心只看保存角色和冻结输入，不链接三个模型。
5. `publishEncodedArtifact` 保持一份 reserve/provideEncoded/回滚算法；各领域自由函数只负责准确的产物检查和调用。
6. 不把 source-save 的 origin 继承用于 derived artifact；不让发布产物更新作者 checkpoint。
7. 完成/取消/Unknown 记录和确认协议保持，不能用 TaskId 替掉 WriteTicket/SaveId。

#### 必须回归

自撤销 describe、递归 accept/ack、回调内收集别的编码完成、不同 W1 确认时机的版本衔接、Unknown 阻塞 lane、真实文件冲突/删除、关闭后迟到结果、Save As 高水位、Export Copy 不清基线。通过旧 P05/R1/R2 真实测试复用，不新增模拟保存替代。

### 4. scene 运行和投影活动

- RunStore/Controller/RunInspectAccess/StartRunOperation 归 activities/scene；inspect API 的窄 render_client 依赖按已有资格保留，不扩成 render_runtime。
- SceneProjection/Hub 归同包领域活动；视口本地 camera/output/highlight 归 workbench/viewport。
- 保持一份 SceneRuntime、一轮 driveFrame。活动只提交 prepare/adopt/stop，不内部驱动另一帧。
- 实际实例回收和单步结果仍由 Runtime/lifetime 权威管理；RunStore 不复制 32 项结果表。
- SceneProjection 的测试若链接 viewport，迁到后配置集成测试，不能使 production scene_projection 反向链接 workbench。
- 原在 scene/projection CMake 中定义的后端绑定测试按测试能力归组，保留真实 backend 检查，不能改为只检查 key。

### 5. material/flow 活动

将源保存与编译/预览功能放同一领域主题，但保留实际 CPU/toolchain/GPU closure 的 target。**同一目录不等于一个 target 或一个总 Service。**

保留：编译 operation 不可复制/不可按值移动；服务拥有有界 operation；内部 completion state 可以共享；固定源、环境、设置、target key 不变；关闭视图不取消应用仍拥有的任务。

Flow retryLink 继续使用原 object，不能为了统一模板重新捕获当前图。不同 linker/object format 的平台能力判断保留，不能只换可执行文件名。

MaterialPreviewStore 与编译服务继续分担实际不同事实，不合并为 CompilePreviewManager。

### 6. project：移除原 UI/旧编辑链接，只保留真正项目活动

1. 将 ProjectStorage、ProjectOpenData、ProjectCreation、ProjectPublication、ProjectAssetSource 的实际副作用源码归 activities/project。
2. 依赖 authoring/project 的同一个 Model/Manifest/Builder，而不是重声明目录或 ModelChanged 类型。
3. 逐处检查对旧 `editor_editing/editor_assets/EditorContext` 的依赖。只为 EditorError 这样的纯头存在的链接，改为该头的真实 provider；不能把旧 editing 目标挂进活动允许集。
4. `editor/assets` 逐文件处理：源读取/项目导入归 project 活动，具体格式角色归对应领域，原已废弃 TAssetSave 接线不进入新正式路径。
5. 纯函数和资源绑定被旧产品继续消费时可以由旧产品依赖新目标；禁止新目标为了旧窗口包含 transition 私有头。
6. 新的 project 活动必须在没有 workbench 的消费工程中读真实项目数据、产生有界请求/结果。不能为了 CPU 消费把正常错误路径删掉。

若仍存在无法在本轮解开的完整旧用户流程，只留在原旧产品岛；必须说明哪一段是旧流程，不能将“整个新的 ProjectStorage”标成豁免以绕过层规则。

### 7. workspace：文件活动不是桌面执行器

迁入真实 Store 与旧格式只读迁移。保留稳定 ID、目标摘要、selected 恢复范围、中断重试及用户新格式修改。

不新增 `applyTo(ViewHost&)` 或让 Store 持 IViewHost。文件读取后产出既有纯 Layout/Plan/Manifest，application 将它们交工作台。P12 才闭合完整恢复产品用例，当前已有纯 effects 测试仍保留。

### 8. L2 出口

- TaskMonitor CPU 独立消费者真正链接运行，无 view_api/ImGui。
- 活动策略核心和具体角色/后端的边可从 target/编译数据库区分。
- authoring 不依赖 activities；activities 不依赖 workbench/application。
- 原 late completion/撤销/代际/错误 payload/code 保活行为保持。
- 不为了“所有活动集中”创建 ActivitiesManager 或全局 service table。
- 对仍暂留的旧项目流程逐文件给出 P12 删除责任；禁止在新目录放空壳调用旧实现。

---

<a id="unit-06"></a>

<!-- 合并来源：phases/L3_WORKBENCH.md -->

## L3：工作台收拢，具体工具消费作者与活动，不再拥有它们

前置：L2 活动已独立。这里迁移的是已实现正式新 UI 和 interaction，不重写 ImGui/node-editor，不进行 P12 产品入口切换。

### 1. 物理归属和构建边界

| 原位置 | 新位置 | target 处理 |
|---|---|---|
| desktop | workbench/desktop | 保留 ViewHost/desktop shell 实际 CPU/GPU 边界 |
| views/api | workbench/desktop 的公开 View 契约 | 保留现有 view_api，只有一个真实头集合 |
| views/viewport | workbench/viewport | 保留 editor_viewport；不引入作者 Session |
| widgets | workbench/widgets | 原 GraphCanvas/NodeCanvas/TreeRows 单一实现 |
| tools/scene/interaction | workbench/scene 的 interaction 文件组 | scene_interaction 仍 CPU |
| tools/material/interaction | workbench/material 的 interaction 文件组 | material_interaction 仍 CPU |
| tools/flowforge/interaction | workbench/flow 的 interaction 文件组 | flow_interaction 实际名称以 L0 为准，仍 CPU |
| tools/*/ui | 对应 workbench/领域 | UI 与 interaction 不混成一个 target |
| project/ui | workbench/project | 只迁 ProjectView/AssetPicker，不携带 CatalogModel |
| tasks/ui 的剩余 TaskView | workbench/tasks | tasks_ui 消费 E2 Monitor |
| tools/settings 的真实纯 UI | workbench/settings | Context/插件安装接线不混入控件库 |
| editing/sinclude 的 InteractionDelivery | workbench/sinclude 的局部共有算法 | 两工具 PRIVATE 可见，不安装或新增 Manager |

同领域中少量协作文件优先 `include/src/test` 内归组；保留 interaction/ 子目录是实现选择，不要求每一小组有自己的 include/src/CMake。

### 2. Desktop 与 View API

#### 2.1 保留的公共能力

IViewHost/DetachedView/ViewInfo/ViewCloseResult 使用 L1 确认的唯一类型。ViewHost 继续独占完整 DetachedView；Root 只登记对象树；DesktopShell 借用原 Process/Renderer，不拥有 Session/Run/Save 的 authority。

#### 2.2 特殊成员和销毁顺序

- DetachedView 和一次性挂载准备只能移动，不能复制。
- 移动赋值先组成旧拥有单元再释放旧 Pane/code，不能先换旧 code 再调用旧 Pane 析构。
- Activity Operation 的不可复制/不可按值移动约束继续保留；只有外层 unique_ptr 可以转移。
- 对象在自身线程销毁；不能在信号回调栈中物理删除自身或祖先。
- 外部持有 token 销毁时，内部提交的局部记录仍活到通知返回。

#### 2.3 关闭结果不可再粗暴压成 BUSY

临时 Busy 保持同一关闭记录等待；永久失败记录原 domain/code/message 且不无界自动重试；已挂载事实不因通知失败改写为未挂载。Host 只使用准确关闭结果，不知道所有领域 error variant。

迁移中错误头拆分只影响 include/provider，不改变关闭协议或生命周期算法。

### 3. 通用 viewport 与 widgets

`ViewportPresentation/ViewportElement/CameraNavigation/相机输入` 继续共享一份通用实现。作者放置/模型创建等 Scene-only 计算留 workbench/scene 或相应领域，不为 Material 使用 viewport 引入它们。

GraphCanvas 继续调用用户既定的 ax::NodeEditor；它只处理 CanvasNode/Pin/Link、屏幕交互和本地 ID，不拥有领域图/History/ContentStamp。将 UI 事件转成领域 edits 的位置是具体 View。

P10Q 已建立的画布身份安全整理保持：
- 只在安全点重建；有在途编辑不非法整理；
- 保留真实作者布局、pan/zoom/selection；
- 不复用旧 UI 身份去命中新作者对象；
- 不访问第三方私有实现头；
- 不因为目录迁移扩大无界保留表。

### 4. 具体工具 UI 的三个数据状态

每个 View 只允许拥有语义不同的三类内容投影：
1. display 的只读已提交快照及 stamp；
2. `NodePropertiesDraft{based_on,value}` 等可取消本地草稿；
3. 有界排队输入 `{payload,based_on,phase}`。

它们都不是新的作者 authority。普通刷新不能覆盖草稿来源；BEGIN/PREVIEW/COMMIT 的校验保持 P10 R1 规则；BUSY 不重新取 current 或重新 begin；STALE 不每帧自动 rebase。

#### 具体需删除/避免的成员

- 若原 `edits_`、单独 `properties_base_` 或 `draft_base_` 已在 P10 R1 删除，不得为了新模板方便恢复。
- View 不恢复 `compile_task_ / compilation_result_ / save_pending_ / run_instance_` 等已迁出业务 owner。
- 允许必要的显示状态、已看到结果 ID、错误呈现；它们不能成为第二个 service state。
- 关闭一个 View 不停止仍由应用活动持有的保存/编译/Run。

### 5. CPU interaction 在 E3 的规则

物理位置属于工作台，不增加 ImGui/Pane/Root/Vulkan 依赖。它们继续只依赖领域模型、原运行 inspect 需要的纯契约和 edit gate。

选择同步：同完整内容戳下复用已验证身份存在事实，但每次仍经过 Store 访问和 gate；内容变化、重载、关闭、代际变化失效。不能把 selection_source_ 放宽成 HistoryId 或 bool initialized。

临时 BUSY 或错误线程不能触发旧身份清理；只有明确 STALE_SESSION 走失效路径。析构可能执行插件代码的 payload 仍在恰当的读取准入内清理。

### 6. 属性生成器、Inspector 与扩展 UI

- 生成器脚本、模板、support 与对应消费目标一起迁至 workbench/scene，不能只动 C++ 文件。
- 生成输入路径从原源目录重定位到新真实目录；保持 schema、生成符号和实际数据语义。
- 生成头不是另一个源权威；删除旧输出后应能从正确依赖重建。
- 如果代码生成需要编译历史路径，更新实际 generator 工作目录/命令，不建立旧路径软链或全局 include。
- ComponentEditorRegistry 的 UI 工厂语义归 E3；纯配置值/schema 归 E1/现有 modules；plugin 贡献安装归 E4。P11 尚未实现的动态注册不在此时伪造。

### 7. Tests 不反向污染库

新的跨 Material/Flow/Scene/Host/GPU 测试代码放 `editor/tests/integration/`，或先将测试定义延后到该入口。所属文件可为领域 fixture，但 target 必须在所需生产库配置完后创建。

例如原 scene/projection 测试使用 editor_viewport：这是测试组合 E2+E3；不要把 editor_viewport 加到 scene_projection 的 PUBLIC_LINK_LIBRARIES。依赖检查应区分 test 与 production 源角色。

`cmake/installed-consumers` 可保持原位置，因其承担全仓安装测试，不为了根图统一再移动无关目录。只更新实际依赖、源码定位、模式和新安装入口。

### 8. L3 验收

- 真正的三个 interaction CPU consumer 可独立编译/运行；不借 ImGui/Root 的偶然传递头。
- widgets/viewport 不包含具体源模型；Material 不链接 Scene UI。
- TaskView 和 ProjectView 读取唯一提供者，关闭视图不破坏 Model/Monitor/任务。
- 真实 Root 的离树、挂载、通知、卸载、重绑定失败与全局用户改动保全通过。
- 新 SceneView 双视口 GPU/原生输入、Material/Flow 正式属性/画布来源路径按影响运行，不以旧窗口或手工模型 API 代替。
- 没有重复的根 UI/通用 viewport 代码；旧纯算法原体退出，必要旧转换仅限原消费者。

---

<a id="unit-07"></a>

<!-- 合并来源：phases/L4_APPLICATION.md -->

## L4：装配边界、根构建和旧产品退场路径

本批只整理现有组合、准确归属与文档；不会借机实现 P11 动态命令系统或 P12 完整新产品。不要把目标设计中的未来类名全部写成空实现。

### 1. application 的实施边界

允许在这里组合：原引擎上下文和执行/渲染设施、SessionStore、项目活动/目录、WriteCoordinator、保存/编译/Run 服务、TaskMonitor、DesktopShell/Host、现有静态工具工厂。

只有真正的组合/实例化 CPP 可以同时 include 具体后端和具体工具创建入口。其他层通过构造接收必要的引用/窄能力，不接收整个 application 或可遍历的服务表。

不新增：ServiceLocator、ApplicationContext::get<T>()、抽象工厂工厂、反射按名字查找任意服务、超大模板参数的 TEditorApplication。

固定生命周期、构造后必需的依赖优先引用/直接成员；确有失败可选能力才 optional/unique_ptr，并由工厂整体返回失败，不能构造半成功应用再让后续到处判空。

### 2. 当前旧 app/launcher/context 怎么处理

本设计区分 **已存在的新集成装配** 与 **尚未切换的旧产品入口**。

- 现有正式新模块集成测试继续不安装成第二产品。
- 可独立抽出的真实启动/依赖组合代码迁 application，只有被实际 consumer 使用时才建立生产 target。
- 旧 Editor/EditorImpl、EditorContext、PaneManager 的行为型代码在 P12 切换前可留原位置，明确标记旧产品岛；不整包改名为 EditorApplication。
- launcher 的原项目创建 UI 若直接依赖旧 Context，先留旧岛；可直接复用新公开契约的部分迁 workbench/project 或 application/launch。
- 不为根目录数量把旧 app/context/transition 移到 application/legacy；不添加新兼容 facade 让新链依赖旧产品。

当前这一批允许根目录仍有明确的旧入口目录。**最终五层目标不等于本批能跳过 P12 的完整切换。**

### 3. metadata/plugins 按职责拆分，不整体搬目录

每个真实类型/函数作以下四分：

| 实际行为 | 所属 |
|---|---|
| 纯配置值、作者字段描述、不需要 UI 的映射 | authoring 对应域或已存在的 engine/modules schema |
| 创建/绘制具体 UI 控件的 factory/registry | workbench 对应域或通用桌面契约 |
| 创建保存、编码、运行编辑活动的角色 | activities 对应域 |
| 调原插件装载并安装上述贡献、固定注册版本 | application/extensions |

旧 `EditorPlugin/CommandRegistration/PaneRegistration` 若仍承担待 P11 替换的完整策略，只能保留旧消费者，不能在 E0 放一个通用 EditorPluginCatalog 让所有层依赖。

真正的 DLL 装载、平台 API 仍由 engine/project/plugins 提供。新贡献的代码 owner 由现有 lease 贯穿回调、任务、payload 和 destructor；不重写 loader。

### 4. 根 CMake 的最终与过渡形态

正式部分应只配置：

```cmake
add_subdirectory(editing)
add_subdirectory(authoring)
add_subdirectory(activities)
add_subdirectory(workbench)
add_subdirectory(application)
# 原测试能力/BUILD_TESTING 规则保持，按真实配置进入 tests。
```

旧产品的少量受控配置可以在 root 一个显式“P11/P12 到期”段保留，或复用现有迁移 CMake 片段。**这是列举已有 targets 的施工配置，不是新的兼容层或新的产品 option。**

原则：
1. 不再在 root 逐个枚举新 tools/*/model、persistence、interaction、ui。
2. 每层负责层内真实 target 的拓扑顺序。
3. 同层 API/值先于实现；跨层集成测试最后建。
4. 若只为 target 尚未配置而出现旧顺序例外，调整构建定义，不引入 source 逆向边。
5. 不用根 GLOB_RECURSE 自动编入所有源，包括旧文件；生产 source 列表必须可审阅。
6. layer 不是五个聚合库；不得创建五个链接全部子库的 INTERFACE 总目标来掩盖实际使用。

### 5. 根文档必须同时说明三件事

`editor/README.md` 重写为正式长期入口：
- 当前已落地新五层、所有权和依赖图；
- 原产品尚未切换的事实与明确旧目录/target/消费者；
- P11/P12 的下一入口，而非声称这些未来用例已存在。

删除默认指导“Pane 拥有业务源和历史”“普通新工具都直接借旧 EditorContext”等与新目标冲突的表述；旧行为留在明确历史/旧产品小节，不与正式规则并列为两个同等方案。

更新 AGENTS/开发说明中的源路径、生成器入口、实际目标清单；历史 dev_log 文本不要全局替换。模块 README 写长期职责，施工日志仍在 .internal/dev_log。

### 6. 真实跨层流程检查

沿生产代码或正式集成装配检查以下链，不增加新流程状态机：

| 链 | 装配应保证 |
|---|---|
| 编辑 → 编译/预览 | View 的输入保留 based_on，Session 提交；编译使用冻结源，结果仍服务所有者 |
| 保存 → View 关闭 → 完成 | 关闭只移除 View，Save/Task/Write 继续由原 owner 结清 |
| Run → 停止 → 晚查单步 | Runtime 回收重实例，Run 通过原结果表晚读取直至确认 |
| 读取布局 → 纯计划 → UI准备 | 活动不知 Host，application 传值；本批不新增完整 P12 ApplyLayout |
| 项目目录替换 → 多 View | Model 只一份数组/索引/修订，通知作为刷新提示，读取仍核对版本 |

旧 C01/C03/C04 不改判定，也不新造同编号豁免。

### 7. L4 出口

根 CMake 与 actual graph 能对应五层；新正式路径不会落回旧 Context。应用不提供通用服务查找。所有暂留旧文件有实际消费者与 P11/P12 期限；没有空目录、空模块和“未来再填”的默认成功函数。

无需立刻把所有旧产品目录迁到最终五层，但长期新源码的去向必须全部落实；不能以等 P12 为由继续把 TaskMonitor 留在 UI 包或把新模型放旧工具目录。

---

<a id="unit-08"></a>

<!-- 合并来源：phases/L5_ABSTRACTIONS.md -->

## L5：在真实复用点使用 C++20 concept，动态边界只保留一次

前置：L1–L4 的新 owner 和依赖已清楚。本批不以新增概念数量作为成果。详细代码契约见 `03_CONCEPTS_AND_DYNAMIC_BINDING.md`。

### 1. 必做：约束现有 InteractionDelivery

原共享算法存在于 P10Q，实际被 MaterialView/FlowView 使用。L3 将其归 workbench 后，L5 完成：

1. 定义两个小的内部概念：`VoidDeliveryResult<R>` 与 `DeliveryAction<F,R>`，只要求算法真实用到的构造/调用/Result 能力。
2. 以 Validate 的 Result 类型为共同结果，对 cancel/begin/preview/finish 返回完全相同类型作约束。
3. 函数仍只持当次栈上的 actions，保留原 stage 参照；不拥有 queue/payload/stamp。
4. 原 `EInputDeliveryStage` 值保持，成功才推进阶段；失败保持原阶段和错误。
5. 不给算法增加泛化 DomainKey、TaskId 或全局调度功能。
6. 不能在共同 helper 把错误变成 bool，或一律映射 BUSY；两个领域沿原准确错误返回。
7. 两个工具都使用这一份函数定义，不能一边迁入新 concept 版一边保留旧裸模板旁路。

### 2. 必做：抽象清单与动态边界审查

每个现存 Port/Provider/Controller/Operation/大模板列清：
- 是否有独立事实/不变量/寿命；
- 运行时是否允许未知实现；
- 实际静态实例化者；
- 是否只是转发同一对象的方法；
- 是否把两种错误/结果混成一个 action。

结论只允许 KEEP_CONCRETE、KEEP_DYNAMIC、CONSTRAIN_STATIC、MERGE_FUNCTION、DELETE_FORWARDER、DEFER_P11/P12（必须已有旧消费者）。不能全部 KEEP 且不解释。

明确保留：IEditSession 的异构 Store，ISaveSource 的运行角色，适当的 IEncodeJob/IPreparedRebind，运行时未知 factory 的一次边界。它们不是为了测试而模拟出的多态。

### 3. 条件做：FrozenEncoder

若现有三类编码 job 只有同一控制骨架、差异仅为 snapshot/codec，并且至少两种真实编码可共用，则将共同核约束为 FrozenEncoder，在真实 job 构造/注册处实例化一次，再由既有 OwnedEncodeJob 擦除。

不满足条件就不做，记录具体差异，例如 Scene 多文件快照包、Material 图编码、Flow 冻结值具有不同输出和预算语义。不要为了“concept 必须用两次”添加对称 Codec 包装类或新 Result。

原接口已足够正确时，使用一个普通 concrete API 是抽象，不必补 `ISceneSession` 或 `TSaveService<...>`。

### 4. 继承、组合和 variant

- Pane/Element 的继承体现 UI 节点可替代性；LuxObject 体现真实活动对象的线程/信号，不为所有纯值加入对象基类。
- Session 与 Source/History/State 使用组合；不存在 SceneSession→MaterialSession 的继承。
- View 必需服务用引用/不可空构造；真正 Unbound/Author/Running 用现有 variant 表达。
- 控制 owner 的特殊成员显式；领域结果可以复制，控制责任不能顺便复制。
- 无状态函数不要包 Operation 类；已有 Q10 删除的 Publish*Operation 不得复活。
- 不为减少 void* 把低层合法 ABI/类型擦除机制改成多层 std::function；只整改普通业务中没有必要的擦除。

### 5. 运行时语义不能交给 concept“证明”

编译器约束可以拒绝错误返回类型/调用形式；不能证明 payload 深层拥有、预算真实、schema callback 不重入、内容身份仍有效、代码 owner 尚存、文件没有被别的进程修改。

因此保留：factory 构造不变量；跨回调/异步/下一帧/注册撤销的重验；文件最终发布前冲突检查；Runtime/Host 保护；已接纳完成事实的可靠接收。

删除冗余检查必须记录首次证明点、使用范围、失效事件、实际消除的工作和对应回归。不要做“if 数量减少”统计作为目标。

### 6. 概念资格

在当前已验证 C++20 编译器上：
- 两种真实 View 的实际 actions 是正例；
- 一个 action 返回 bool、另一 action 返回不同领域 Result、错误 cv/ref 可调用、Result 不满足 void 语义均为对应负例；
- 负例必须先能 include 当前相关真实头，不可把缺头/缺第三方库当 concept 拒绝；
- concept 放内部 sinclude 时，源码契约测试使用精确内部授权；SDK 消费者仍通过公开 MaterialView/FlowView 验证真实实例化，不强行把私有 helper 安装出去；
- 不使用本包独立说明性片段作为 Lux SDK 的运行证明。

### 7. L5 出口

共享算法只有一份、有两个实际使用点、明确语义律、正反编译证据。动态抽象没有被重复包装；模板没有出现在 application 的全服务树上。所有删除的转发入口都有真实消费者迁移记录。

---

<a id="unit-09"></a>

<!-- 合并来源：03_CONCEPTS_AND_DYNAMIC_BINDING.md -->

## 抽象、C++20 concept 与动态边界施工规格

本章展开原设计，不引入第二套框架。代码分为“可直接按现有算法整合的契约示例”和“可选设计”，不得把示例中的替代类型复制成新的生产类型。

### 1. 机制选择表：先问变化在哪里

| 实际问题 | 采用 | 不采用 |
|---|---|---|
| 稳定唯一实现，只需隐藏内部 | 普通类API/private/PImpl | 自动配一个I类和转发Adapter |
| 编译期已知多种类型共享算法 | 小函数模板＋concept | 整个服务树模板化 |
| 封闭的运行时绑定集合 | 现有variant＋穷尽visit | any＋字符串tag＋到处downcast |
| 运行时未知类型异构拥有 | 现有窄virtual或一次owning erasure | virtual外再套多层function表 |
| 已提交事实/控件事件 | LuxObject TSignal＋Connection | 第二observer/event bus |
| 当前调用栈内的行为借用 | 具名callable/template/function_ref | 捕获进下一帧仍用借用函数 |
| 跨帧可调用工作 | 已有Task/角色/owning callable | 用function_ref规避分配后悬垂 |

concept不保证性能，virtual不自动构成设计缺陷。必须说明实际调用频率、开放集合、对象寿命和编译依赖。

### 2. 必做：现有 deliverInput 的局部约束

#### 2.1 目标文件和可见性

真实目标：`editor/workbench/sinclude/lux/engine/editor/workbench/InteractionDelivery.hpp`。

仅为Material/Flow具体UI实现的同步交付核。两个target以精确PRIVATE include路径消费；不把所有workbench sinclude传到SDK，不让authoring或widgets使用它。C++ namespace改为 `lux::editor::workbench::detail`；旧editing路径及using别名全部删除。

#### 2.2 契约头示例

以下类型名用于该私有实现，不是新的公开ABI。生产Result继续采用两个View现有的 `lux::cxx::expected<void,E>`。不要求改原错误enum。

```cpp
#pragma once
#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

namespace lux::editor::workbench::detail
{
    template<class R>
    concept VoidDeliveryResult =
        std::default_initializable<R> &&
        std::move_constructible<R> &&
        requires(R& result, const R& observed)
        {
            typename R::value_type;
            typename R::error_type;
            requires std::same_as<typename R::value_type, void>;
            { static_cast<bool>(observed) } -> std::same_as<bool>;
            { result.error() } -> std::same_as<typename R::error_type&>;
        };

    template<class F, class R>
    concept DeliveryAction =
        std::invocable<F&> &&
        std::same_as<std::invoke_result_t<F&>, R>;

    enum class EInputDeliveryStage : std::uint8_t
    {
        BEGIN, PREVIEW, COMMIT, CANCEL, COMPLETE
    };

    template<class Validate, class Cancel,
             class Begin, class Preview, class Commit>
    requires std::invocable<Validate&> &&
        VoidDeliveryResult<std::invoke_result_t<Validate&>> &&
        DeliveryAction<Cancel, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Begin, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Preview, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Commit, std::invoke_result_t<Validate&>>
    auto deliverInput(
        EInputDeliveryStage& stage,
        bool commit,
        Validate&& validate,
        Cancel&& cancel,
        Begin&& begin,
        Preview&& preview,
        Commit&& finish) -> std::invoke_result_t<Validate&>
    {
        using Result = std::invoke_result_t<Validate&>;
        if (stage != EInputDeliveryStage::CANCEL &&
            stage != EInputDeliveryStage::COMPLETE)
        {
            if (auto result = std::invoke(validate); !result)
            {
                return result;
            }
        }
        if (stage == EInputDeliveryStage::CANCEL)
        {
            if (auto result = std::invoke(cancel); !result)
            {
                return result;
            }
            stage = EInputDeliveryStage::COMPLETE;
        }
        if (stage == EInputDeliveryStage::BEGIN)
        {
            if (auto result = std::invoke(begin); !result)
            {
                return result;
            }
            stage = EInputDeliveryStage::PREVIEW;
        }
        if (stage == EInputDeliveryStage::PREVIEW)
        {
            if (auto result = std::invoke(preview); !result)
            {
                return result;
            }
            stage = commit ? EInputDeliveryStage::COMMIT
                           : EInputDeliveryStage::COMPLETE;
        }
        if (stage == EInputDeliveryStage::COMMIT)
        {
            if (auto result = std::invoke(finish); !result)
            {
                return result;
            }
            stage = EInputDeliveryStage::COMPLETE;
        }
        return Result{};
    }
}
```

这是按当前已读函数整理的**整合示例**；实施者必须在真实lux-cxx版本和实际Material/Flow Result上编译。若当前Result正确但traits不符，调整概念到真实必要能力，不能改全部Result或添加桥只为满足示意代码。

不机械加入 `noexcept`：现有copy/载荷转换的异常政策必须保持，只有真实不抛契约才用requires noexcept。

#### 2.3 语义律（编译器不能代替的部分）

- 每个action只借用当前调用栈，函数不保存它们。
- 调用者在产生请求时固定based_on；本函数不重新取current，不拥有或复制作者模型。
- action失败返回原Result，失败阶段不前移；成功阶段不因下一阶段BUSY重复执行。
- validate不是永久授权。后续action跨扩展回调或内容边界时，仍用原领域gate/expected检查。
- CANCEL/COMPLETE沿原规则处理；不得把所有终态无条件重新begin。
- payload析构、失败队首清理及oldcode保活依旧由原具体View/interaction负责。
- 普通Preview不写作者编码；一次Commit产生一次历史。

#### 2.4 正负例要求

正例：实际Material UI actions和Flow UI actions，分别编译运行原 `draft_source_*`、BUSY阶段恢复、取消、stale、Undo/Redo路径。

负例至少涵盖：cancel返回bool；preview返回不同的expected错误类型；action只支持&&却在同步借用&上调用；返回`expected<int,E>`而非void。先通过相同头的正例，再要求负例在约束处失败。错误文本不必绑定编译器逐字措辞，但必须能定位目标约束，不能是缺头/包。

本私有头无需为了编译负例安装。编译契约test使用明确内部路径；安装消费者走公开View并链接实际实例化。两类证据分别记录。

### 3. 可选：FrozenEncoder 的静态复用

只有真正共用流程时实施。原设计的FrozenEncoder只表示：`codec.encode(const Snapshot&,limit,stop)`返回现有PersistenceResult<EncodedArtifact>，snapshot提供对应ContentStamp。

不要求添加无用 `SceneCodecAdapter/MaterialCodecAdapter`，也不要求让三种不等价的格式硬统一。优先直接约束实际可调用codec函数对象，在领域实现CPP实例化，再交给现有OwnedEncodeJob。

需明确：
- 最外层code owner、snapshot/codec的成员声明顺序及移动赋值；
- 原消费输入在READING内清理的保证；
- frozen bytes的budget语义和SharedBytes拥有关系；
- stop/error不变成成功；
- job采用基线仍在owner，不在worker；
- source-save与derived publication不是同一来源链。

若共享只剩三行调用，保留原具体实现更简单。L5记录“不引入此概念”的理由是合规结果，不因此阻塞；不得将可选项扩张为新框架任务。

### 4. 动态边界保持窄而真实

#### 4.1 IEditSession

Store需要管理运行时未知Session，保留虚析构/描述/关闭私有契约。动态角色不需要暴露所有领域编辑，具体工具继续使用typed SessionKey及公开领域API。

#### 4.2 ISaveSource / IEncodeJob

SaveService不知道具体Scene/Material/Flow仍可接收角色。code与payload生命期由原OwnedEncodeJob/registration承担。一个动态边界足够；不增加IJobPort→Adapter→Facade→IEncodeJob。

#### 4.3 ViewFactory / 插件

未知工厂使用当前必要多态；concept只在源码扩展的编译期检查，不是插件ABI。P11才完成动态注册/版本生命周期，当前不伪造成功的PluginManager占位。

C ABI/稳定类型擦除若已有真实用途，不因“禁止Adapter目录”而删除。禁止的是长期同义体系互相补偿，不是实际跨ABI边界。

### 5. 实例化放置与公共依赖

- generic定义只include必要正式值/Result和标准traits，不include全部具体Session/codec。
- 具体实例化位于对应活动provider或工作台CPP；application只选择组合，不承担所有内置模板实现。
- 内置组合若显式实例化能实际降低编译负担可采用；不能限制合法外部源模板扩展。
- 若必须看所有后端才能编译策略头，依赖倒置没有成立；不能以“模板会优化”掩盖。
- 公共PImpl头只保留真实值/Result所需完整类型；引用/指针服务采用前置声明。不要为每个前置声明再建一个forward-only API库。

### 6. C++20与基建纪律

固定C++20，不无条件引入std::expected/std::move_only_function/std::scope_exit等更晚库接口；用当前已验证lux-cxx设施。不使用std::span/string_view保存悬垂借用，不跨frame缓存Registry/Pane裸引用。

新concept/type的完成条件是：真实用途、语义律、consumer/实例化点、编译与行为证据、未引入多余依赖。没有这些就应删除，而不是将其当未来可扩展性资产。

---

<a id="unit-10"></a>

<!-- 合并来源：04_BUILD_AND_DEPENDENCY.md -->

## 构建组织与实际依赖门禁施工规格

本章规定怎样让五层从目录变成可检查的源码事实。继续扩展现有Editor架构检查与CMake File API，不建设新的通用构建分析平台。

### 1. 四张表应一致

| 表 | 最少字段 | 实际用途 |
|---|---|---|
| source/header归属 | 文件、层、角色、逻辑include、定义owner、target | 防止物理移走但旧头仍编译 |
| target声明与实例化 | SOURCES、private/public includes、实际编译依赖、模板实例化者 | 查include/生成/模板穿透 |
| link/安装闭包 | 直接及传递边、imported targets、LINK_ONLY、export/find | 防止静态链接和SDK漏声明 |
| exe/DSO装载与owner | 真实产物、运行库、共享身份/元信息位置 | 避免静态复制唯一全局状态 |

不是要求每张表一份新数据库；可以作为唯一账本和现有File API输出的不同视图。

### 2. 明确允许方向

- E0只使用基础设施与纯值。
- E1使用E0与对应纯领域能力，不依赖E2/E3/E4。
- E2策略使用自己的契约；E2具体provider实现可使用对应E1与引擎技术能力，不依赖E3/E4。
- E3通用UI不依赖具体作者模型；E3领域UI可以使用E1/E2公开契约和通用E3。
- E4组合正式能力，不能通过私有状态绕过任何层。
- engine/modules不依赖Editor；test target独立标记，不作为production边。

“workbench不依赖活动实现”具体指：不include后端私有头、不持活动内部状态、不直接选择/构造与其业务无关的文件后端；**不是说最终静态链接不能包含SaveService的函数定义。** 公共服务普通类的实现必须链接，静态库传递依赖真实存在。不能以最终exe里有FilePublication就判UI穿透，亦不能以PRIVATE名义忽略政策target里的反向include。

### 3. layer与真实闭包标签分开

每个target至少标明：`editor_layer`（E0..E4或TEST/RETAINED）、`role`（VALUE/POLICY/PROVIDER/INTERACTION/UI/COMPOSITION/TEST）、必需能力（CPU/PROCESS/TOOLCHAIN/GPU/PLATFORM）。字段可并入现有rules模型，不新增产品运行状态。

例子：
- scene_model=E1/PROVIDER/CPU；
- editor_persistence=E2/POLICY/CPU；
- editor_persistence_execution=E2/PROVIDER/PROCESS；
- editor_tasks=E2/PROVIDER/CPU+PROCESS；
- material_interaction=E3/INTERACTION/CPU；
- GraphCanvas所属widgets=E3/UI，不得引入作者ContentStamp；
- 新双视口test=TEST/INTEGRATION/GPU+PLATFORM。

不能仅按物理层全量授权，例如“E3允许UI，因此interaction也可以链接ImGui”。

### 4. 施工模式与最终模式

在现有 `rules.json` 增加 `editor_layering` 规则组，使用一份精确source/target映射。

施工期允许已列出的旧产品targets处于RETAINED。每一个旧target要有原消费者、退出P11/P12、禁止新入边。未知文件/新target默认REVIEW，不作为native leaf忽略。

L6最终模式：所有新正式target必须有层及角色；原新路径迁移完成；只有账本中的原旧产品岛允许RETAINED。不能留下“所有editor/tools/*都豁免”的粗规则，因为它会放过新代码。

阶段仍P10Q，不改变正式阶段序号。历史check_receipt按原SHA的rules执行，不因为当前层次变化改旧判定。

### 5. CMake组织步骤

1. 保留原add_component/lux_classify_target/安装工具能力；不要复制一套Editor专用宏系统。
2. 每层CMake显式列真实子领域，保证提供者先于消费者；公共契约/纯值先于实现。
3. 同主题必要的多个targets在同一CMake定义，SOURCES明列，不根目录递归收集。
4. 跨层tests延后到editor/tests/integration配置；不能把尚未定义target当普通-lfoo蒙混链接。
5. PUBLIC仅用于公共头真正暴露的依赖；PRIVATE仍记录实际closure。
6. generated source依赖指向产生文件的真实custom target；不依赖碰巧已安装的旧头或第二轮重编。
7. install exports包含静态库解析所需的传递依赖；不要删除find命令后用开发机器PATH补齐。
8. 同一target不编入两份逻辑相同的CPP；注意旧生成meta注册和新直接实现重复。

### 6. target/包名与动态库决定

物理迁移默认保持准确的原target/逻辑include/package，减少无意义兼容成本。新真实任务CPU分割可引入editor_tasks；旧tasks_ui仍编真实View，不是alias。

只有确实删除/合并旧模块时才删除其包入口；所有消费者一起改，不留下empty INTERFACE转发包。

不全局改BUILD_SHARED_LIBS。已有History/Session共享身份、metadata/反射注册、插件跨DSO需要共享owner的边界保留；内部新实现默认STATIC。禁止whole-archive、export-all-symbols解决静态注册或missing symbol。

检查来自exe和插件的身份是否同域、code/delete是否同有效期。此项本轮继承既有边界资格，不提前设计新的P11 ABI。

### 7. 真实禁止边夹具 N01–N14

每行都需“正常配置/编译 → 加边被指定规则拒绝 → 去边恢复”的记录。可扩展原test_editor_boundaries等同一个测试程序，**无需每行一个新项目模板框架**。

| ID | 人为注入 | 预期拒绝 | 正向对照 |
|---|---|---|---|
| N01 | E1 material include MaterialView或工作台私有头 | authoring_outer_dependency | 原MaterialSource/codec允许 |
| N02 | E0链接任何具体Session model | editing_domain_dependency | 原纯身份/History允许 |
| N03 | SaveService core链接三模型具体provider | persistence_policy_concrete_source | 单独SceneSaveSource target可合法链接SceneSession |
| N04 | WorkspaceStore include IViewHost/Root | activity_workbench_dependency | DockLayout/ViewInfo纯值可使用 |
| N05 | widgets include ContentStamp/SceneSession | widget_authoring_dependency | CanvasEdit/UI IDs允许 |
| N06 | TaskMonitor经tasks_ui引入Pane/ImGui | task_monitor_ui_dependency | 原Process/Object允许 |
| N07 | MaterialView使用scene_ui取得viewport | cross_tool_ui_dependency | editor_viewport允许 |
| N08 | CPU interaction直接或传递引入GUI | interaction_capability_leak | 对应authoring允许 |
| N09 | engine/modules链接任一Editor target | product_reverse_dependency | PLAYER原依赖闭包允许 |
| N10 | PUBLIC/INTERFACE或LINK_ONLY隐藏N01 | 与直接边同一规则 | 删除同一真实边恢复 |
| N11 | 模板策略头include全部后端 | policy_instantiation_leak | provider实例化CPP可以包含具体后端 |
| N12 | 新正式库include旧transition/context | new_legacy_dependency | 旧产品消费新正式API允许 |
| N13 | generated header来自旧隐藏路径 | generated_provider_mismatch | 实际新generator输出且依赖已声明 |
| N14 | 未分类新target/未解析imported依赖 | unclassified_dependency | 显式归属真实leaf及版本后通过 |

模板和header扫描要结合编译器依赖输出；仅regex看到词汇不能当语义证明。检查器无法处理的genex/imported边报告未知并定位，不默认放过。

### 8. 最小能力消费者

除原SDK矩阵外，以下可以放入现有消费者目录作为额外模式，不要求新建六个库：
- E0＋任一真实作者模型：无需workbench或编译后端开发包；
- layout纯值/plan：无Root/Provider callback；
- TaskMonitor：无Pane/ImGui的创建、observer、结果查询；
- SaveService core：用真实保存契约测试，不链接三种具体模型；
- 三个interaction：CPU层，不启动窗口；
- widgets/viewport：根据本身实际图形需求，但不带具体作者工具。

这里的“无需”是实际配置/编译/链接闭包，不是仅没调用相关构造函数。不能关闭生产功能来模拟依赖消失。

### 9. C++与平台审查

检查actual compile flags确为C++20而非默认23；无跨平台不支持的扩展关键字；Windows头、API、user32和链接参数在适当分支。文件大小写、UTF-8路径、资源生成工作目录、Windows/ELF linker format应按原资格保留。

Linux仍NOT_RUN不阻塞，但不能据Windows编译成功宣称Linux支持。目录迁移不得继续强制普通CPU test找lld-link；工具链测试只在相应能力启用时配置。

---

<a id="unit-11"></a>

<!-- 合并来源：05_ACCEPTANCE.md -->

## 验收矩阵：观察事实，不追求测试数量

下列 XL01–XL24 是观察主题，不是要求新增24个可执行程序。优先在原测试/SDK消费者加入必要断言或重新登记迁移后的场景。原行为可以复用证据方法，但最终修改过的实现必须在最终SHA实际运行。

### 1. 总判定

本轮PASS要求：五层归属真实、必要依赖负例命中、原行为不缩水、Windows及实际适用UI/GPU/安装通过、删除项闭合。

Linux/系统IME未测单列NOT_RUN；用户取消补满的旧慢算法样本保持原PARTIAL历史，不阻塞本轮。新增故障不能借未测平台或旧C编号掩盖。

### 2. XL01–XL06：输入与纯内容

#### XL01 用户改动与历史快照

**动作：** 核对实际HEAD、输入SHA、用户文件bytes/diff。迁移后对比原工作区；对旧dev_log树执行Git对象差异检查。

**断言：** 原用户改动没有被stage、覆盖或丢弃；移位的tracked ProjectBuilder与用户补丁分别记录；旧快照无改动；前置与实现有清楚祖先关系。

**证据：** 两个工作区状态、旧/新路径对应、raw hash与tracked blob不同口径、历史tree/diff。

#### XL02 editing唯一性与异构Store

**动作：** 用真实Scene/Material/Flow在同Store中创建；保存typed key，关闭并复用slot；验证历史/ContentStamp/许可。

**断言：** 不同模型独立历史；旧代际拒绝；current不来自重复缓存；关闭的无分配内容戳查询保持；IEditSession仍只必要多态。

**结构断言：** 原History/Session定义在新E0真实路径只一份，旧源不编译、无forward header；不链接具体模型到E0。

#### XL03 三模型CPU独立消费

**动作：** 分别构建只使用安装E0+单模型的消费程序，实际编辑/捕获/撤销重做；审查configure与link闭包。

**断言：** 无Process、Runtime/composition、ImGui、Root、Vulkan、compiler/linker或旧Context；允许实际纯描述/schema/graph/codec。

**不能代替：** 完整Editor里未创建窗口不算独立闭包。

#### XL04 作者状态和原子编辑

**动作：** 原Scene混合删除重建+字段、Material节点替换、Flow变量/签名/连接的正负批次；Undo/Redo与NO_CHANGE。

**断言：** 完整源编码、history cursor/revision、observed、dirty、binding/checkpoint符合原规则；失败不部分写入。不是只看返回值。

#### XL05 身份与输入清理

**动作：** 原Flow高水位/耗尽测试、重载输入无绑定/BUSY/clone失败、codec读取重入编辑、最后code owner释放。

**断言：** 旧ID不命中新对象；正常Redo/显式恢复有效；数据先于code析构，READING外层不被嵌套操作解除。

#### XL06 纯项目与布局

**动作：** ProjectBuilder验证合法/非法配置、Catalog共享快照与失败保留；LayoutPlan输入纯inventory，检查未挂载Root与dirty作者。

**断言：** Builder没有任务或IO（D01），项目纯target不因错误归类链接Process；plan无provider/open/rebind副作用；ViewInfo同一定义；ViewClose错误不反向引入workbench。

### 3. XL07–XL14：活动与可靠结果

#### XL07 保存策略与具体角色分离

通用SaveService core consumer只认识ISaveSource/OwnedEncodeJob/WriteCoordinator，不include三种Session或FileArtifactStore。具体三个角色分别使用真实源捕获/采用。审查两个编译单元和链接闭包，不能只看目录名。

#### XL08 保存与基线

捕获S10后继续编辑S12；发布S10，current仍S12且dirty。SaveAs保留History/作者ID/Flow发号高水位；ExportCopy不改checkpoint。关闭Session并复用槽位后，迟到published事实保留，不采用到新会话。

#### XL09 回调与完成运输

复用P05/R1/R2的真实角色：describe撤销、accept嵌套adopt/ack、accept内collect别的编码完成。确保不会UAF、重复accept、丢完成或永久ENCODING；内层完成后外层dispatch仍有效。Task/TaskScope路径不可用手动shim替代正式资格。

#### XL10 同目标写入与共享字节

乱序编码仍按票据发布；失败空洞正确结清；Unknown直到writer退休/结果确认才释放lane；已确认旧回执不破坏同源合法版本衔接；不同origin/外部变更冲突仍成立。原SharedBytes保留同一owner与预算，不重新复制大payload。

不重新跑所有大字节计时；保留1个真实产物和代表大小的owner/limit断言即可，除非本轮实际改了传输算法。

#### XL11 Run与结果寿命

实际start/pause/step/stop；完成一张未确认，再排队两张停止，实例回收后第一次读取应得到COMPLETED/CANCELLED/CANCELLED。FAILED包含原payload/code寿命；单项/Run最终确认后失效。没有第二drive或由RunStore猜测结果。

#### XL12 编译与预览

Material两个内容版本乱序完成，旧结果不覆盖新目标；最新失败显示准确stale状态。Flow链接失败后修改作者图，再retry仍使用原编译object。operation不可复制/按值移动，结果可共享；关闭View不取消服务仍拥有的编译。

#### XL13 TaskMonitor脱UI

在真实Process上创建Monitor、不创建Pane/ImGui；多个订阅者观察任务变更，目录共享同一revision快照；一个订阅销毁不影响任务；取消由Runtime准入。实际安装/链接无view_api、desktop、GUI。TaskView另测借用同一Monitor，不各自抢observer。

#### XL14 Workspace文件与恢复范围

旧Alpha/Beta合法布局都迁移，只selected的内容进入RecoveryManifest；空/失效选择和IO/BUSY分类保持；目标已存在同源用户修改不覆盖；marker前中断可重启。真实文件写/删继续共享WriteCoordinator，不能通过直接remove绕过在途责任。

### 4. XL15–XL19：工作台

#### XL15 通用widgets与viewport

GraphCanvas使用原node-editor，领域Graph/History不进入widgets。真实画布有界整理后作者布局、pan/zoom、选择和旧UI ID失效仍正确。Material与Scene共享viewport而非互相链接工具UI；测试后端真实按ViewHandle隔离。

#### XL16 interaction CPU与访问错误

三类interaction不需要窗口编译/运行。回收另一个Session的cleanup回调访问仍活目标时BUSY保持手势/选择/来源；selection-only也保持。真实STALE才清理旧身份；wrong thread保持原错误。不要把错误归一成成功或BUSY。

#### XL17 正式UI草稿/排队来源

实际Flow属性草稿S0，外部合法提交S1，display刷新后Apply应拒绝旧草稿，原S1完整编码/历史不变；Revert后新捕获可正常提交。Material/Flow Canvas信号在S0入队、BEGIN前S1变化同样拒绝。BUSY恢复同一phase/payload/based_on，不重启和rebase。

不能用手工给模型传旧expected的单元测试替代这条真实UI缓存链。

#### XL18 离树/挂载/关闭

真实Pane/Element在离树构造不注册Root；容量/工厂失败保留旧UI；提交后通知再请求关闭进入后批；payload/挂载token析构不让执行栈悬垂。DetachedView移动赋值先清旧节点再释放code。永久关闭错误停止自动重试，BUSY保持原记录。

#### XL19 新双视口与原生输入

正式SceneView+ViewHost+DesktopShell，两View同作者、独立相机/尺寸/高亮；从一个编辑另一Undo；关闭一窗只退休它的资源。保留实际GPU输出回读/拾取/validation日志。Windows原生输入/capture/焦点按原脚本经过新桌面。两个旧显式GPU模式分别记录，不冒充新双View。

IME没运行就NOT_RUN。编译默认CPU模式的consumer即使目录叫GPU也不能计GPU通过。

### 5. XL20–XL24：概念、依赖和工程收尾

#### XL20 概念的真实正反编译

Material/Flow真实actions是正例，共用同一deliverInput。错误Result、bool返回、仅&&可调用、非void expected等负例命中约束。内部概念不为测试强装进SDK；SDK使用公开View链接实际生产实例化。不能说concept证明snapshot深层拥有。

#### XL21 动态边界与代码寿命

原Store异构角色、保存撤销、encode job错误/取消/最后owner、View factory所有权仍通过。检查同一对象只经过一个必要动态边界，不新增虚接口后再包函数表。共享身份/元信息必要DSO边界不被STATIC改动复制。

旧C03按P11原故障合同保留；这不豁免本轮新工厂或保存边界自己的生命周期回归。

#### XL22 安装、头和生成

全新install prefix；改动后的public headers逐个C++20独立编译；正向SDK模型/活动/工作台按真实需要链接；旧已删除headers/package不参与。删除生成输出后能正确再生，第二轮无工作；无法从旧SDK偶然找到缺失include。

#### XL23 依赖结构

运行N01–N14；检查源/target/安装/实例化四张表一致。实际未知边不能默认为叶；对同夹具修复后的正例也运行。新层源不能依赖原Context/transition。测试依赖不得倒灌production。

#### XL24 删除、旧岛与文档

每条MOVE/DELETE都有Git差异、consumer更新和旧文件退出；SPLIT每个符号有唯一新位置。零消费者旧target/头删除，不留alias。旧产品剩余清单有P11/P12责任且无新增消费者。根README区分新链与旧入口；.internal是唯一施工材料，历史收据原样保存。

### 6. 失败时如何处理

| 情况 | 正确动作 | 不允许 |
|---|---|---|
| 新代码正例失败 | 保存第一次日志，修代码/真实配置后重跑 | 改断言弱化含义 |
| negative因缺包失败 | 先恢复正例依赖，再测试禁边 | 将非零退出当PASS |
| GPU/系统能力不可用 | 标BLOCKED或NOT_RUN，说明是否当前必测 | 偷换CPU/Fake模式 |
| Linux没环境 | 按用户范围NOT_RUN，不阻塞 | 报跨平台通过 |
| 原历史C01/C03/C04仍失败 | 保留原结果/责任 | 挂上新的本轮缺陷延期 |
| 旧性能样本没满 | 保留66/100历史与范围变更 | 伪造剩余样本或重启无关长测 |
| 用户修改冲突 | 保留bytes、隔离tracked实现、报告重定位 | reset或纳入本轮提交 |

### 7. 计数与结果汇总

结果清单应按XL主题映射实际test名/命令，而不是要求总数固定。可以保留一个行为映射文件：old_test→new_test→old_assertions→new_assertions/重定位说明。修改公开API导致测试源码变化时说明等价观察，不盲目要求字节相同；未变测试体则原样保留。

最终记录一个完整结果向量：结构PASS/FAIL，Windows各组，安装各组，GPU各模式，Linux/IME/ASan范围，旧故障，性能是否涉及。总PASS不覆盖NOT_RUN，也不把允许延期的未测项重写成成功。

---

<a id="unit-12"></a>

<!-- 合并来源：phases/L6_QUALIFICATION.md -->

## L6：同一最终 SHA 的资格、删除检查与交付

本批验证前五层已经落实，不再借验收临时重设计所有模块。任何修复继续追加实现提交，最终只绑定一个新的 implementation_sha。

### 1. 最终状态冻结顺序

1. 检查全部 L0–L5 去向、target、抽象和旧消费者处置已闭合。
2. 导出最终 tracked diff，逐文件确认没有用户原修改被带入。
3. 提交最终实现，记录完整 SHA；在独立干净检出和新构建/安装前缀建立资格。
4. 显式 P10Q，启用结构严格模式，不能停在 L0 全旧路径豁免模式。
5. 运行构建/回归/安装/实际模式；发生代码修复就形成新 SHA，并重跑受影响与最终资格，不能用旧构建成绩覆盖。
6. 实现稳定后冻结 dev_log/P10Q-structure，独立证据提交；不把证据自引用 SHA 写入自身哈希循环。

### 2. Windows 工程验证

- 复用 P10Q 已验证的工具链和精确第三方版本，生产保持 C++20、禁不必要扩展。
- 从实际收据取得完整配置和依赖参数，不猜配置变量或仅用手写一个 cmake 命令替代 SDK 闭包。
- 新 build tree 首次全量构建，随后第二轮无工作；首次失败与修复日志保持。
- CPU-only 模型/interaction/TaskMonitor/纯活动 consumer 不要求启动/链接 UI；实际 backend closure 分开核验。
- PLAYER 保持无 Editor 编译/安装输入；不能仅以没有创建编辑器窗口作证。
- 所有新的 public header 独立 include；C++20 负例含原八项 operation 特殊成员拒绝。
- 原第二编译器可用时保留，其不可用如实标记，不把 clang-cl 当 Linux。

### 3. 功能与真实桌面

从实际 baseline 测试清单构建行为映射。原报告数字 204/204、CPU178/178、PLAYER11/11 仅是索引，不要求新版本保持相同总数，更不允许减少行为。

最终至少涵盖：三作者/所有历次 R1-R2、保存/真实IO、Run/晚读结果、workspace/selected迁移、交互BUSY与来源、Host生命周期、目录/任务、compile/固定对象retry、GUI graph/input。

旧 GPU_UI、EDITOR_SCENE_PANE 和新 SceneView 双视口须按真实配置和命令区分；更新文件位置不能使模式回落默认 CPU_UI。新双视口需要仍有真实输出/隔离/拾取/退休观察和 validation 记录，不以空白输出或纯 key 表替代。

Windows 原生输入继续按当前可用设备和既有脚本运行。系统 IME 候选/组合/提交若无实测仍 NOT_RUN；Unicode 注入不是 IME。

### 4. 安装消费者

- 全新 install prefix，不读取旧 build tree 的头/DLL。
- 原全部适用 SDK consumer 使用真实公开入口；测试源若移动，变更 source path 而不借内部实现头。
- 新增/调整 TaskMonitor CPU consumer；新 E0/E1 纯值/模型独立 consumer；通用保存策略不链接具体模型/文件后端的 consumer。
- 静态 dependencies、PIC、导出宏和旧 DLL 搜索路径由实际 link/loader 结果验证。
- 安装清单不能含旧转发头、旧已删除 alias 包、测试访问、内部 sinclude。
- 若只移动物理源而公开逻辑 include/target 没变，这不是“没整改”；验证它指向唯一真实文件与正确层。

### 5. 依赖与负例

执行 `04_BUILD_AND_DEPENDENCY.md` 的真实正反夹具。每一负例均需：基准正例成立 → 加非法边命中目标规则 → 去除同一边正例恢复。未知 imported leaf、LINK_ONLY、生成 include、模板实例化不可以漏掉。

不要让 consumer-only 阴性测试因为缺少 Vulkan/LLVM/SDK 目录就“通过”。

### 6. 性能与跨平台范围

- 不补旧 50k 深链到100样本。
- 保留短的真实复杂度/原子性/容量/SharedBytes owner/画布整理回归；纯目录迁移不重新跑 BQ1–BQ5 全计时。
- 算法或数据结构若在本轮变化，只测对应路径、记录规模/机器/分配范围，不能用比例推算成绩。
- Linux 当前没有环境：源码/平台分支/路径/CMake规则审查写明，构建运行为 NOT_RUN；不阻塞按新用户范围验收。
- ASan 与全量 SDK 插桩不一致造成的未资格仍如实保留，不关闭检查掩盖链接失败。

### 7. 删除检查

按 file_plan 和 symbols 作肯定式核对，而不是只 grep 名字：
- 原源码不再参与 compile，旧物理文件已删除或明确暂留。
- 原逻辑定义恰有一份；无同名 ODR 副本/重复生成注册。
- 被删除的层级/包没用 alias 或 include 壳恢复。
- 新目录没有原 Context/PaneManager 旧业务的改名副本。
- 保留的旧产品文件有真实消费者、没有新增消费者且 P11/P12 期限明确。
- 不删除历史收据中的旧名称，grep 报告区分 active source 与 dev_log。

### 8. 证据搬运与哈希

归档检查继续只读相对路径和固定 Git 对象。复制归档到含空格/中文路径后校验；缺必需真实日志、篡改日志必须失败；恢复原 bytes 后成功。

这证明归档自洽与可迁移，不证明未测平台通过。不要要求历史 verifier 针对新路径验证旧源码；每份收据始终用自身 implementation_sha。

### 9. 出口判定

以下全部满足才标记 `P10Q-structure: PASS`：
- 五层长期代码去向闭合，没有无法解释的逆向生产依赖；
- 修改中的 owner/invariants 原行为回归成立；
- 真实 Windows/SDK/相关 GPU 与输入通过；
- 文件、符号、target、安装、文档、模板实例化图一致；
- Linux/IME/历史性能范围单独列明，不冒称完整跨平台；
- 当前原产品与新链状态明确，P11/P12 尚未实现内容没被伪称完成。

最终摘要采用 `07_RECEIPT_AND_RESUME.md` 格式。正常推送既有实施分支后停下等待复审，不自动实施 P11。

---

<a id="unit-13"></a>

<!-- 合并来源：06_FOLLOWING_PHASES.md -->

## P11–P13 持续约束与交接

本章用于更新后续实施文档，不授权当前LLM提前进入后续阶段。旧阶段编号、原功能义务和既有故障责任保持，物理路径按新五层解释。

### 1. P11：命令与扩展

| P11工作 | 唯一位置 | 必须遵守 |
|---|---|---|
| Command查询/Invocation/DispatchReceipt | activities/commands | 固定目标与注册版本，不依赖当前焦点补目标 |
| 会话生命期契约 | editing | 不把所有领域功能塞IEditSession |
| 作者格式/codec贡献 | 对应authoring或原schema | 静态约束不替代运行时code寿命 |
| 具体保存/编译角色 | activities/对应域 | 真正格式/采用职责，不套多层Adapter |
| ViewFactory/控件贡献 | workbench对应域/desktop契约 | 只构造完整DetachedView，不私自open内容或直接挂Root |
| plugin加载与贡献安装 | application/extensions | 调原engine loader，不自建平台装载器 |

完整P11的命令代码寿命、注册撤销、批次快照和动态工厂仍需实际实现/验证；本施工不会以一个concept demo代替。C03责任仍在P11。

concept用于真实静态实现的约束，未知插件在真实异构边界动态绑定一次。普通fixed组合可直接类，不要给每个动作配I/Provider/Port/Adapter。

### 2. P12：完整产品和旧框架删除

- 不需要UI的SaveAll、关闭内容决定、重载和恢复进度归activities。
- `OpenAndShow/ExitEditor/RestoreWorkbench` 等跨内容与Host/平台的组合归application，持有组合进度而非复制底层状态机。
- ViewHost执行挂载和布局，WorkspaceStore只文件，authoring/layout只纯计划；禁止E2 include E3来图省事。
- 唯一产品入口改用正式新链，旧EditorContext、PaneManager、旧三大Editor、到期transition及旧注册入口同阶段删除。
- 不把旧target改INTERFACE链接全部新库保留旧名字；不创建永久legacy；不通过Old/New option长期双轨。
- 旧文件格式和用户数据只读迁移保留，数据兼容与旧业务框架是两件事。
- C01完整旧布局应用、C04启动失败责任在P12按原合同结清。

ProjectBuilder.cpp用户差异继续保护到它被用户授权整合；旧工作区存有diff不等于可丢弃。

### 3. P13：验证与最终收敛

P13不为未删除的旧框架新增延期。实际用户声明支持的环境才报告支持；当前Linux无环境仍不能声称支持，但也不恢复成每轮开发硬阻塞。获得环境或要正式承诺Linux时再建立真实资格。

真实系统IME、完整产品退出、安装、DSO边界、GPU退休/像素/输入和持续运行按适用范围验证。不重复已经被用户停止的旧算法长测，仅测需要决策的新回退或变化。

### 4. 后续代码审阅的七个问题

1. 新文件属于哪一层、哪个领域？是否有新的混合顶层？
2. 是否直接使用原LuxObject、Process、SceneRuntime、RenderRuntime、toolchain、lux-cxx？
3. 新类型承担什么独立事实/生命周期？没有就用函数或合并。
4. 此抽象为何静态/动态？concept有真实算法和实例化点吗？
5. 源码/API/模板/link/装载依赖是否一致？
6. 旧入口是否真实删除，或有被授权的有限consumer/期限？
7. 行为与性能证据是否支持结论，未测项有没有被夸大？

出现新core/services/runtime/adapters等顶层需要重新说明，不允许以功能多了为由恢复旧混合组织。

---

<a id="unit-14"></a>

<!-- 合并来源：07_RECEIPT_AND_RESUME.md -->

## 施工接力、证据与最终交付格式

### 1. 唯一账本，不复制状态

`.internal/editor-redesign/` 保持施工权威。附带JSON模板只表示所需字段，可合入原账本，不必为每一类另建一个管理工具。阶段末冻结至 `dev_log/P10Q-structure/`，冻结之后不再把它作为活动工作目录。

原P10Q的PARTIAL保留。新增scope amendment写明：用户已取消旧50k样本补足和当前Linux实测硬阻塞；Windows、适用GPU、依赖和原行为仍要求实际验证。不是把原receipt.json中的PARTIAL改PASS。

### 2. 每批最小交接消息

```text
P10Q-structure / Lx
输入：<完整SHA>
实现：<完整SHA或明确尚未提交>
已闭合：<具体责任与文件/target>
删除：<旧定义/源/target；没有则写无>
暂留：<真实消费者、P11/P12责任>
验证：<实际命令及结果；未跑项目>
用户修改：<原路径/新映射；原bytes状态>
下一入口：<唯一下一L批次>
```

不能只写“重构完成，测试全绿”，也不需要每个小函数都附一份重复报告。

### 3. L6最终冻结目录

```text
dev_log/P10Q-structure/
  README.md                    # 实际结果、范围、唯一下一入口
  receipt.json                 # input/implementation/evidence与命令索引
  scope-amendment.json          # 新用户范围，原历史不改
  file-plan.json               # 全量文件和split符号去向
  target-map.json              # 真实层/角色/闭包和二进制决定
  abstraction-map.json         # 静态/动态/保留/删除理由
  behavior-map.json            # 原行为→当前实际测试
  retained-product.json        # 原旧岛consumer与P11/P12退出条件
  logs/                        # 命令、退出码、stdout/stderr
  evidence/                    # 实际File API/编译/安装/模式结果
```

可按原工具保留等价命名，不需要重复同一JSON内容。禁止把新报告塞生产include/src或安装SDK。

### 4. SHA与哈希的含义

- input_sha固定原参考与实际工作前置。
- implementation_sha只含此次被验收tracked代码，不含用户未授权diff。
- evidence_commit在实现之后单独提交；不在其自身文件里要求填写自己的Git SHA，避免自引用。
- file blob来自对应Git对象；raw SHA256用于运行产物/用户原bytes，两者不能混用。
- 哈希一致说明文件身份，不说明测试正确或运行过。
- 原absolute机器路径只为溯源，checker实际读取相对归档路径和Git对象。

### 5. 最终对用户的摘要模板

```text
P10Q-structure 已完成／PARTIAL，停在P10Q。
实现：<sha>；验收：<sha>。
已完成：五层正式归属、真实闭包、TaskMonitor分离、输入交付concept、旧路径退出。
保留：<必要动态接口/共享DSO边界/原owner>。
实际验证：Windows ...；SDK ...；GPU各模式 ...；依赖/编译负例 ...。
范围：Linux NOT_RUN不作为本轮阻塞；系统IME ...；旧长测不补跑。
原C01/C03/C04：<原判定与责任>。
用户修改：<实际保全状态>；main未改。
仍需复审/下一入口：<不自动进入P11>。
```

### 6. 失败和中断

正常编码/目录迁移/构建失败保留首个原始记录。只有旧判断错误才改测试，必须解释原测试错误与生产修复的区别；不能删掉第一次失败然后把重跑记为首次通过。

若一次LLM任务只能完成部分：提交可构建的已闭合单元，明确剩余计划和未通过项，停在当前L编号。不要临时压缩后续任务为“移动完成”；不要在未验证代码上发最终PASS。

复工只读真实HEAD、账本、上次结果和用户改动；不从长对话的印象推断已完成，也不重新跑已确认无关的旧长测。

---

<a id="unit-15"></a>

<!-- 合并来源：appendix/SOURCES_AND_LIMITS.md -->

## 本施工包的来源、裁定和制作边界

### A. 已认可设计（原字节附带）

- D1 `reference/00_LAYERED_DESIGN.md`：五层职责、静态/动态选择、依赖矩阵、范围。
- D2 `reference/01_MIGRATION_AND_ACCEPTANCE.md`：原迁移计划L0–L6与后续阶段。
- D3 本对话用户对Linux/旧长测范围的明确调整：本包继承，不用旧收据重新否定。

本包不是再次设计第六套架构。`01_DECISIONS_AND_INVARIANTS.md`对ProjectBuilder的显式纠正以当前代码为依据，其余目标语义与D1/D2一致。ViewError拆分、私有输入算法路径、任务target名字等为本次施工细化，不声称原代码已有。

### B. 本次补读的固定Git来源

所有源码下列URL都固定在 `f7c27f9375cbf8dd8af37b30a6027a460de26213`，除ref查询本身。

| 编号 | 路径／资源 | 用途 |
|---|---|---|
| S01 | Git ref `refs/heads/codex/editor-redesign-v4` | 本次仍指向上述验收SHA |
| S02 | P10Q用户报告（本对话README(10).md） | 204/178/11等历史验证范围、用户文件保护、实际依赖SHA |
| S03 | editor/contracts/include/lux/engine/editor/views/ViewInfo.hpp | 纯观察与关闭错误混合定义 |
| S04 | editor/project/include/lux/engine/editor/project/ProjectBuilder.hpp | 纯Builder公共结构 |
| S05 | editor/project/src/ProjectBuilder.cpp | 验证配置并返回，不执行异步/IO |
| S06 | editor/project/CMakeLists.txt | editor_project实际source与STATIC边界 |
| S07 | editor/editing/sinclude/lux/engine/editor/editing/InteractionDelivery.hpp | 已有单一交付算法与可约束签名 |
| S08 | editor/tools/scene/model/CMakeLists.txt | 纯模型source/PersistenceAccess/真实测试 |
| S09 | editor/tools/scene/projection/CMakeLists.txt | 生产与跨层集成测试依赖区别 |
| S10 | editor/tools/material/ui/src/MaterialView.cpp（开头范围） | 真实共享交付头consumer、工具依赖 |
| S11 | editor/editing/include/lux/engine/editor/EditorError.hpp | 纯错误声明不等于旧业务目标 |
| S12 | editor/editing递归Git tree | 新旧editing与sinclude文件位置 |

源码链接基址：

```text
https://github.com/LUX-YU/lux-engine/blob/f7c27f9375cbf8dd8af37b30a6027a460de26213/
```

### C. 本次没有完成的工作

- 没有修改或推送lux-engine仓库代码。
- 没有对全仓每个文件完成语义审查；71条路径规则是从已读设计和关键代码构造的计划种子，必须在L0展开并核验。
- 没有重跑引擎构建、CTest、SDK、GPU、Linux或完整历史归档checker。
- 没有将原P10Q PARTIAL改为PASS。
- 附带脚本的测试使用本地合成Git仓库；只证明计划输出、安全拒绝和规则处理，不证明引擎依赖正确。
- 目录/target/类型裁定是施工目标，不是已实现的代码事实。

### D. 文档和辅助工具的验证

制作过程将检查本包链接、JSON、原设计字节一致性、计划脚本的合成fixture以及ZIP完整性。记录见 `evidence/package-selfcheck.json`。

如果另有独立C++概念检查，其范围只能是合成状态类型的约束/阶段逻辑，不是实际lux-cxx/Material/Flow/Process或SDK资格。正式实现必须运行本施工定义的真实consumer。
