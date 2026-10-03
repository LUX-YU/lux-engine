# EC1 完整施工总册

本文件由同包 00–07 正文依次拼接，内容无另行改写。分批实施以各章和总册的相同规则为准。

# EC1：共享基础能力、开放扩展与数据／执行语义收敛

**类型：可交付给实施 LLM 的下一轮设计与施工规范。日期：2026-10-02。**  
**审阅基线：`54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8`。**  
**当前状态：设计交付；未实施，未运行引擎资格。**

## 0. 本轮解决什么

这不是撤销 P13 验收，也不将旧免验项目改成必测。EC1 是用户针对后续扩展和代码表达提出的新增收敛工作。

目标不是“再开放一个画界面的回调”，而是：内置和外部开发者均能直接组合正式的资产查询、冻结读取、会话、历史执行、窗口构造／登记、命令、任务和发布能力；需要完整文档流程时，再组合这些能力。宿主不再靠三种内置编辑器的类型分支接通产品。

同时，以实际行为而非句式决定数据与执行者的边界。`Graph / GraphCompiler / CompiledGraph` 是一种适用形态；`EditHistory / EditExecutor / ApplyResult` 是本轮针对历史的目标形态；`ViewHost.adopt(view)` 和纯函数 `planLayout(...)` 仍然合理。不得把所有 API 强制变成同一套三段式。

## 1. 阅读顺序

1. 本文件：范围、不可破坏的事实和停止条件。
2. `01_FINDINGS_AND_SCOPE.md`：已确认问题、同类扩展及不应开放的封闭部分。
3. `02_SHARED_PRIMITIVES_AND_ROUTING.md`：细粒度公共能力、注入方式、开放路由和数据格式。
4. `03_DATA_EXECUTION_AND_TYPES.md`：历史分离、行为主语、状态与结果类型。
5. `04_CONTEXT_APPLICABILITY.md`：2D／3D、分区、组件、Feature 和工具的统一判定。
6. `05_AGENTS_AUDIT.md`：AGENTS 逐类核对与规则冲突的处理。
7. `06_IMPLEMENTATION_AND_ACCEPTANCE.md`：S0–S7 批次、删改范围、测试与交付。
8. `07_SOURCE_INDEX.md`：固定源码、上传规范和证据边界。

编号用于文档导航，不意味着必须创建相同数量的生产模块、测试程序或 CMake target。

## 2. 优先级

用户本次需求与上传的 `AGENTS.md` 为本轮依据。上传文件原字节保存在 `references/AGENTS.user.md`。

五层及其依赖方向继续使用：

```text
editing → authoring → activities → workbench → application  （语义由内向外）
application / workbench / activities 的实现向相应内层公开契约依赖；内层不得反向依赖外层。
editor → engine → modules；editor 可直接使用 modules。
```

`docs/editor-quality.md` 的 QR01–QR22 继续适用。旧施工材料中“本阶段不改 History”“不改变格式”等范围限制不是永久架构约束；EC1 明确授权的历史职责分离、公共契约和必要格式演进以本文件为准。未被明确改变的行为及限制不自动失效。

不能用通用 C++ 指南悄悄替换用户的排版和命名规则。特别是复杂判断的具名语义分组、120 列和异常政策必须按 AGENTS 执行。

## 3. 硬边界

- 不新增 EditorExecutor 线程池、EditorRuntime、私有事件总线、第二套 ECS、文件发布队列或 History 算法。
- 不新增顶层 `sdk/ports/adapters/managers/common` 来放置职责不明的内容。公共能力在真实 provider 的 `include` 中。
- 不建立 `getService<T>()`、`map<string, any>` 或万能 EditorContext；插件获得显式、按职责分组的依赖。
- 不把“无需改宿主”解释为插件可直接越过 owner 线程、准入、Root 安全点或 WriteCoordinator。
- 不要求所有内置类插件化。只有明确承诺开放的变化方向进入本轮 OCP 整改。
- 不以目录数、类数、concept 数、虚函数数或测试数量为完成标准。
- 内置实现不得保留一条绕过公共能力的特权业务路径；应用装配可以构造具体提供者，但不能私自替其执行算法。
- 原作者／运行身份、历史高水位、来源戳、保存基线、Unknown 发布、迟到完成和 GPU 退休语义保持。
- 不要求此次实现动画时间轴、IK、重定向、蒙皮工具全集。骨骼编辑器是最小真实扩展验证，不是新增一个完整 DCC 产品。

## 4. 允许的大改与必须有的减法

允许重构不能直接提供干净接口的模块，包括 Application 中硬编码的路由、History 的执行职责和绑定拥有关系。

但每次引入新类型必须同时说明：取代哪一份旧责任、为何不能用已有类型／自由函数、它拥有何种数据、是否跨线程／回调、谁负责销毁。替代完成后删除原分支、转发方法、旧字段、旧头和旧安装出口。

不接受：在 `EditExecutor.undo()` 内调用仍负责全部工作的 `history.undo()`；在新 `DocumentService` 内保留旧三类型 switch；把内置私有对象藏进一个 `void*` 然后宣称 SDK 开放。

## 5. 完成标准

EC1 结束时至少成立：

1. 三种内置作者编辑器与一个真正独立构建的骨骼扩展，使用同一套基础能力和路由。
2. 骨骼扩展可查询／读取资产、创建与登记 Pane，也可以无 Pane 编辑与保存；这些能力不依附某个唯一“编辑器接口”。
3. 打开、保存、默认视图、Save As、恢复与关闭不再必须认识骨骼的 C++ 类型，也不新增第四个宿主分支。
4. History 保存记录与状态；执行者执行原来的唯一准备／提交／回放算法。两个执行者不能绕过同一历史的保护。
5. 组件列表、对象创建、工具可用性及 Feature 配置复用准确的上下文判定；最终领域校验仍独立存在。
6. 已确认的类型表达和可控复杂度问题完成修正；未覆盖项目明确列明，不给出“全仓已完美”的结论。
7. 上传 AGENTS 的适用规则逐项登记，有实际例证；不能仅运行格式器后宣称全部遵守。

## 6. 权限与证据

只交付／实施本轮范围，不自动合并 main、删分支、发布 release 或处理用户未提交补丁。实施起点先检查实际 HEAD 与祖先关系；即使分支以后已合并，也不要求回退到旧 SHA。

唯一可变施工材料继续位于 `.internal/editor-redesign/`；在原账本增加 EC1 部分，不建立第二本。阶段末冻结 `dev_log/EC1/`。原阶段的 FAIL、PARTIAL、USER_WAIVER 和 NOT_RUN 不修改。

Linux／系统 IME 继续按现有未测范围记录；不建设新环境，不补旧 50k 深链至 100 次。新改路径需要适用 Windows、SDK 和生命周期验证，不能把旧免验泛化为“新功能也不用验证”。

本包中的新签名、类型和方法均为目标设计，不是已存在 API。源码事实、设计判断、风险推导和待测结果必须分别标识。


---

# 01　问题清单与开放范围

## 1. 证据分级

- **事实**：在固定 SHA 的代码中直接观察到。
- **判断**：依据用户本次扩展目标作出的设计评价，不是语言标准认定的错误。
- **风险**：合法输入／调用组合的静态推导，尚未在真实 SDK 中复现。
- **待核对**：本次没有完整读取或没有运行证据，S0 必须核对；不得写成已确认缺陷。

[Sxx] 指向 `07_SOURCE_INDEX.md`。本次没有取得完整源码检出的 AST、最终链接产物和全仓运行画像；下列不是穷尽列表，也没有虚构全仓违反数量。

## 2. 本轮应开放的变化方向

| 编号 | 功能模块与事实 | 为什么需要开放 | 必须替代的东西 |
|---|---|---|---|
| O01 | `ProjectManifest.hpp/.cpp` 使用五值 `EProjectAssetKind` 和固定 `kinds` 字符串表。[S04,S05] | 插件新增骨骼等作者资产不能修改宿主枚举和解析器。 | 用现有 `AssetTypeId` 与有版本的来源格式描述表达开放类型；旧 kind 只在只读迁移中映射。 |
| O02 | `EditorViews::openCaptured()` 只映射 Scene、Material、Flow。[S06] | 登记 SessionFactory 后仍不能从项目浏览自然打开新文档。 | 从内容类型贡献解析 SessionKind 和源读取策略；缺 provider 明确报错。 |
| O03 | `EditorSaving::assetKind()/askSave()` 再次识别三类内容、后缀；`prepareSave` 据此拒绝其他会话。[S07] | 新会话有 ISaveSource 仍不能参与项目保存。 | 来源类型、默认命名、项目登记由正式描述提供；SaveService 不识别骨骼。 |
| O04 | `makeContentView()` 为三工具构造交互和输入；`ContentView` 有三类具体指针。[S06,S09] | 插件完整视图绑定、多视图、恢复仍需改 Application。 | 正式 ViewFactory 与内容—视图关联；工具自身组合其局部交互和预览租约。 |
| O05 | `settleRecovery()` 将三种资产 kind 与三种 ViewType 配对。[S08] | 即使插件能打开，重启恢复仍会拒绝。 | 恢复使用与打开相同的关系解析，并保留 exact ViewType 与 RestoreKey。 |
| O06 | `VCompiledSource` 在 Application 中仅容纳 Material／Flow 产物。[S09] | 需要发布自定义派生产物的插件不应改产品 variant。 | 编译仍保留领域类型；发布边界接受拥有型编码／描述／来源记录，复用原协调器。先核对 EditorArtifacts 完整消费者。[S38] |
| O07 | V7 `contribute(Draft&, CodeLease)` 可交条目，但没有按职责注入项目／会话／工作台能力的激活协议。[S10] | 插件不只是创建静态面板，还要访问宿主实际资产、执行器和窗口 owner。 | 在现有扩展装配处增加明确的能力注入与 activation 寿命；不把整个 Application 指针交出去。 |
| O08 | `InspectorView::options()` 枚举全部加载 schema；Outliner 创建选项仅检查 Transform schema 存在。[S18,S20] | 当前内容支持的空间／组件不等于进程中已加载的实现。 | 与域准入共用的适用性查询和诊断，而不是控件各写一份 is2D。 |
| O09 | Feature 有依赖／冲突／设备描述，但 Editor 的筛选没有统一对象／场景上下文判定。[S21,S32,S33] | 插件新增效果或工具需要声明准确需求，不能继续向场景表单加类型分支。 | 复用原 Feature 描述；只补作者上下文的要求与观察，避免第二套依赖解析器。 |

### 不是所有 OCP 缺口都同样严重

“能够登记一个无绑定 Pane”已经有正式能力，不列为从零实现。真正缺的是在宿主中取得合适能力，以及开放的完整内容路由。

O06 仅针对派生产物发布的开放方向。骨骼扩展不必凭空提供编译器；只编辑并编码已有 SkeletonAsset 也能验证作者扩展。

## 3. 明确不列入 OCP 整改的封闭部分

以下不是因为有 enum／switch 就需要插件化：

- 固定版本 wire／文件解析中的标签分支；其变化由显式格式版本处理。
- Execute／Undo／Redo、Save／Save As／Export、调用线程与阶段状态。
- 当前应用运行／关闭阶段、固定回执结果与有限 UI 意图集合。
- 平台实现对 Windows／其他支持平台的编译分支。
- 旧数据类型到新稳定标识的只读映射。`recoveryType()` 的历史 ID 映射本身不是应删的业务 hack。[S08]
- 小型纯算法、一次性许可、固定错误集合，以及不需要替换的 TaskMonitor、ViewHost 实现。
- 本轮没有承诺的任意第三方图节点磁盘序列化。GraphCanvas 能画节点不等于所有 Graph codec 已开放；需要时另按真实需求扩展，不能把本轮偷换成万能图框架。

## 4. 数据与执行者语义问题

| 编号 | 当前表达／事实 | 本轮判断与处置 |
|---|---|---|
| D01 | `EditHistory` 同时拥有条目、游标、阶段、观察者，并执行 prepare/apply/publish/replay。[S01–S03] | 按用户要求分离日志与执行算法；保留唯一历史数据和阶段保护，不只换语法。 |
| D02 | `PreparedSessionData` 实际保存 Prepare／Reload 可调用对象、code 和一次消费语义。[S12] | 不是普通 Data；改成准确的 `SessionPreparation`（目标名）或同等单一准备操作；原数据与回调寿命不能丢。 |
| D03 | `ProjectCatalogSnapshot` 私有 owner，却公开可改 version/name/assets 借用视图。[S16] | 类型允许破坏 owner 与观察的对应关系；改成只读访问器并增加快照上的索引查询。未证明现有调用已误用。 |
| D04 | `ResultIntent` 分离 action 与不相关的 target variant。[S09] | 改成动作与载荷绑定的 `VResultIntent`；避免 ACK_SAVE + StepTicket 这类可构造的无效状态。 |
| D05 | `WorkspaceIntent` 包含 action/layout/label/ticket 的大并集。[S09] | 使用有限领域请求 variant；不是开放扩展点，不需要注册每个动作。 |
| D06 | `optional<bool> structure_request_` 表达无请求／添加／删除。[S18] | 小型明确请求类型或 EComponentAction，不能靠布尔值暗示动作含义。 |
| D07 | `ContentView` 允许同时非空 scene/material/flow，且把各工具装配放进产品状态。[S09] | 在开放绑定整改中消除，不先改成三分支 variant 又让骨骼补第四分支。 |
| D08 | `ArtifactPresentation` 有多组 optional 和 encoding/reading/settled；它是操作进度，不是普通 Presentation 值。[S09] | 先列合法状态与跨状态资源；只对确实互斥部分用命名状态 variant，不把整个 Application 再包成一个 Manager。 |
| D09 | `Snapshot::prepare/create`、`Source::capture` 等混合命名需要按实际副作用分类。[S11,S23] | 只读加工可用自由函数或 encoder/compiler；持有资源的 owner 保留方法。不得对所有类盲目统一后缀。 |

**D01、D02、D07 是职责重构；D03–D06 是类型不变量强化。它们不是已复现的数据损坏声明。**

## 5. 继续扩展发现的同类问题

### 5.1 “注册类型”重复出现在多个后续环节

除上轮已列打开／保存外，本次直接核对到恢复的三类型匹配。[S08] 因此只删 open 的 switch 不足以完成；还必须核对：新建、默认后缀、资产浏览、拖放、SaveAll、重载、关闭、恢复、派生产物及插件撤销。

### 5.2 基础能力已有，但入口仍以产品对象作为汇集点

`ProjectStorage` 同时公开 catalogModel、catalog、catalogRevision、catalogAsset、reference、resolveReference，并持有源读取、cooked reads、发布和关闭。[S15]

这并不意味着 ProjectStorage 必须再拆成十个接口。处置顺序是：保留它真正管理的挂载／项目生命周期；让目录消费者直接使用 ProjectCatalogModel；合并重复转发；把源读取和产物读取语义写清。仅在消费者无法获得干净的必要能力时，提取真实 provider，而不是再增加同义 Port。

### 5.3 贡献公共头通过具体 InspectorView 引入 UI 依赖

ContributionDraft 需要 InspectorComponent，但描述与具体 InspectorView 共处一个头。[S11,S19] 这是契约归属问题，不应靠再加一层导出宏包装解决。将描述放在对应 workbench 公共轻量契约中，具体窗口 include 它；不是把 UI 控件类型搬进底层组件数据头。

### 5.4 查询与保护必须和当前目标绑定

`SceneConfigurationInputs` 带全局注册列表；若界面只以它们推断可用能力，会把“已安装”当作“当前源允许”。[S21,S22] 要求列表查询明确输入内容／对象／视图观察；不得在更新时偷偷改成全局焦点。

### 5.5 底层已有骨骼数据，仍有命名与不变量审查事项

`Skeleton.hpp` 的 `Bone_t` 与用户要求的类型 PascalCase 不一致；它的注释还解释了与 Vertex 中 Bone 的历史概念重名。数据包含 parent_index、bind_local、inv_bind_world，且说明运行单遍处理依赖父骨骼在前。[S41]

这既提供了真正复用的基础，也说明不能为了演示新编辑器随意重排 bones。命名整改可选 `BoneRestPose` 等准确名称，但先核对序列化／生成器／外部引用，不能留下 Bone_t 转发 alias。成员默认值、拓扑与变换关系必须由真实构造／验证边界负责；本次不宣称已经审计全部动画数学。

这一条是新发现的规范与数据语义问题，不要求借 EC1 重写动画系统。

## 6. 可定位的复杂度与热路径问题

| 编号 | 当前路径 | 静态成本／现状 | 收敛目标 |
|---|---|---|---|
| P01 | Outliner 更新后在 collapsed 中逐项查 rows.labels。[S20] | O(C×N)，C 为折叠状态数；依据完整内容戳刷新。 | 折叠用稳定对象身份；一次集合／现有索引检查；依据结构变化失效。 |
| P02 | ModelCreation 每 primitive 用 any_of 检查整个 snapshot.assets。[S25] | O(P×A)，非每帧但大模型／目录会放大。 | 同一不可变快照的 by-id 查询；不能混用 live 目录。 |
| P03 | SceneSource::objects 每对象检查所有 schema。[S23] | O(N×S) 的存在检查，另加实际 codec 工作。 | 统计真实使用规模；按现有实际组件成员／池遍历，不能新增第二权威组件表。 |
| P04 | SceneProjection 只对无变化快返；变化后完整捕获和重建。[S24] | 全量复制、描述编码和实例退休；不是组件级增量。 | 先测变化分类；本轮至少优化无变化／无相关变化工作，是否追加字段增量见 S5 有界裁定。 |
| P05 | capture 的 freezeConfiguration 与 copyOpaque。[S23] | 有重编码／解码和包／卷字节复制。 | 只有证实底层不可变拥有时才共享；不能把 const shared_ptr 当深不可变证明。 |
| P06 | Application／服务链的重复 describe/lookup 与类型分支。[S06,S07,S09] | 尚无当前产品 inclusive/exclusive 采样，不能断言调用层次是首要热点。 | 记录调用次数、分配与实际栈；只合并无失效边界内的重复查询。 |

以上是源码成本分析，不是本轮实测时间。历史 BQ 报告只作为基线背景，见参考文件与第 06 文档。

## 7. 严禁反向修坏已正确的部分

不得为了让代码看上去简单而删除：同源／代际验证，P05 已准入完成吸收，P11 命令与贡献复合保护，控制对象不可复制，History 身份高水位，Unknown 文件结果，拒绝载荷清理范围，Root 挂载安全点和 GPU 退休。

这些约束是公共能力可组合的前提，而不是“阻碍细粒度”的冗余。


---

# 02　同一套细粒度能力：内置与外部共同使用

## 1. 不是发布一个 IEditor，而是发布可组合能力

本轮不以 `drawEditor(context)`、`IAssetEditor` 或一个巨大的 `EditorServices` 作为全部 SDK。

开发者应能只使用需要的子集：无 UI 的资产检查工具；有 Pane 但不打开作者内容的资源浏览器；一份内容多个视图；一个视图同时比较多份内容；有独立历史并参与保存的完整骨骼编辑器。

这些用法不应要求插件自行管理第二 SessionStore、直接修改项目 TOML，或通过 Application 私有成员取得对象。

## 2. 能力目录与实际 provider

| 能力 | 现有基础／归属 | 公共契约要保证什么 | 不应暴露什么 |
|---|---|---|---|
| 资产身份与种类 | modules 的 AssetId、AssetTypeId；authoring/project 描述 | 稳定种类不依赖闭合 Editor enum；来源与派生产物种类区分。 | 运行时 Entity、函数地址、C++ type hash 作为磁盘 ID。 |
| 查询项目资产 | ProjectCatalogModel / ProjectCatalogSnapshot | 同版本不可变数组和索引、准确项目实例、按 ID 查询、变化通知。 | 可被调用者改写的 span/version、目录私有容器。 |
| 读取作者源 | ProjectStorage::captureSource、AssetVfsView | 固定目标、版本／摘要、预算；读取失败不是不存在。 | UI 回调中的同步整文件读取、未受控的 ProjectStorage 内部表。 |
| 读取 cooked 资产 | captureAssetReads、AssetReadPort、Process loadAsset | 使用固定读取端点、真实 codec、取消与拥有型完成。 | 与作者源码混用的无类型字节入口。 |
| 创建／访问会话 | SessionStore reserve/prepare/publish、TSessionAccess | 唯一 owner、完整代际、读写准入、不可见准备、真实角色安装。 | 任意 Registry 可写指针、第二 current/dirty。 |
| 编辑与历史 | 领域 Session / EditExecutor / EditHistory | 领域准备、一次提交、Undo/Redo、冻结输出；也支持无窗口使用。 | UI 直接改变 cursor、执行任意旧 memento。 |
| 创建 Pane 与 Element | modules/function/ui，LuxObject | 真实离树构造、原控件布局与信号；不要求继承额外插件 UI 基类。 | 第二 Root、插件私自驱动消息泵。 |
| 注册窗口种类 | 正式 ViewFactory、ContributionDraft | 类型／版本／绑定、代码寿命、一次构造结果；支持独立工具窗口。 | 固定骨骼/Scene 分支或与主菜单字符串耦合。 |
| 挂载／请求窗口操作 | ViewHost、ViewRequests、IViewHost | 创建、采用、描述、show/focus/close；owner 安全点；结果与请求分开。 | 任意回调立即删除自身、返回无限期裸 Pane 借用。 |
| 命令 | CommandRegistry 与不可变目录 | 目标固定、query/execute 分开、旧版本 pin、后批更新。 | 为获得一个方法而暴露整 Application。 |
| 后台执行 | engine/process | 原 TaskScope、scheduler、取消、进度和完成吸收。 | std::async、自有线程池、新任务状态数据库。 |
| 保存与文件发布 | SaveService、SaveExecution、WriteCoordinator | 源保存、产物发布、项目目录采用各有结果；同物理目标排序。 | 直接 ofstream/remove 绕过在途写入。 |
| 投影／预览 | 原 Runtime、RenderResources、viewport 与领域投影活动 | 使用既有资源与退休；固定来源、视口本地状态。 | 第二 RenderRuntime 或 Window 退出即删共享运行实例。 |
| 布局／内容恢复 | 纯 LayoutPlan、ViewHost、WorkspaceStore、内容打开活动 | 布局只组织视图；内容从独立记录恢复。 | opaque 布局载荷默认授权打开任意资产。 |

以上必须有独立的安装头和最小 consumer，不意味着每行必须新建一个类。现有 API 已够用时直接复用。

## 3. 读取、任务与 owner 的边界示例

目标使用方式如下。它展示职责，不是逐字可编译的当前 SDK 示例：

```cpp
auto catalog = asset_catalog.snapshot();
auto source = source_reader.capture(asset_id, expected_digest, limits);
auto work = task_scope.submit(read_and_decode(std::move(source)));
// worker 只产出拥有型解码结果。
// owner 收到后通过原 SessionStore / 安装协议发布。
```

不能为了少一层，把 `source_reader` 变成存放全项目状态的万能对象。若它只是 ProjectStorage 四个方法的同义转发，应直接使用已有提供者或在提供者内提取真实的读取职责。

返回借用值的有效期必须写在接口上：同步 callback-only、到下一次 mutation、或由 owning snapshot 延长。`function_ref` 不可跨帧保存。返回 span 的快照必须使所有观察始终指向同一个私有 owner。

## 4. 服务如何交给插件：显式注入，不是查全局 Context

### 4.1 两个时点

**登记时**描述能力、格式、工厂、命令和需求，不创建活动 Pane，也不从当前焦点读取内容。

**激活／构造时**由 application 把所需正式服务引用交给插件的激活对象或工厂。激活对象负责自己的 Connections 和 TaskScope 请求，但服务事实仍由原 owner 管理。

当前 V7 出口没有这一显式 host-services 入口。[S10] 必须在现有扩展契约内演进，而不是通过静态全局变量、反射找私有地址或回调先保存 `EditorApplication*`。

### 4.2 有界依赖集合

可以采用几组有明确归属的组合值，例如：

- 活动层组合：资产查询／读取、SessionStore、保存与原 Process。
- 工作台组合：dispatcher、ViewRequests／必要的 host 准备入口、正式窗口注册与命令接线。
- 编译／预览扩展另外明确请求相应现有 provider，不要求所有插件依赖 GPU、LLVM 或完整 application。

这些是明确字段的构造参数组，不是 `get<T>()`。每个模块只接收自己需要的一组；原有具体 provider 无需再包装成接口同名副本。

可以用编译期 `requires` 校验内置工厂需要的字段；运行时插件通过声明版本和实际 ABI 的窄绑定。不得构造一个无界能力服务定位器，再用“可扩展”合理化它。

### 4.3 生命周期

application 拥有实际 provider，随后构造 extension activation；插件产物的 code lease 必须覆盖对象、错误值、闭包、deleter 与弱引用控制块尾部。

停止／卸载顺序：拒绝新业务 → 保留并结清已接受工作 → 卸下相关视图与连接 → 回收 payload → 释放 activation 和代码。无法安全热卸载时明确拒绝／延迟，不要求本轮实现任意热卸载。

进程内能力注入是接口和生命周期约束，不是对恶意原生插件的安全沙箱。不得宣称给予一个窄引用即可阻止插件调用 OS。

## 5. 普通窗口不必成为“作者编辑器”

插件可以直接用原 Pane／Element 组合自己的窗口。只有希望参与内容关联、SaveAll、关闭内容和恢复时，才登记内容关系。

关系至少支持：

| 场景 | 所需关系 |
|---|---|
| 任务窗口、统计窗口 | 无 Session 关联 |
| 同骨骼两视图 | 一个 Session、多 ViewId，各自视图状态 |
| 对比两个骨骼 | 一个 ViewId、多个只读／可编辑内容关联，明确主操作目标 |
| 工厂构造成功但挂载失败 | DetachedView 仍由完整 owner 清理，已发布内容不被错误回滚 |
| 关闭一个视图 | 只结束该视图的交互和关联，不自动关闭 Session 或取消未拥有的任务 |

宿主保存通用关联身份与关闭策略；具体骨骼交互、相机、选择和预览绑定由该实现管理。必要的辅助 owner 可放进已有 DetachedView 的完整拥有单元或工厂结果，避免在 Application 再加一个 Skeleton 指针。

活动层不得为此 include ViewHost。跨内容和 UI 的编排仍在 application/workbench 的既定方向。

## 6. 开放路由：补关系，不再造万能文档模型

目标关系为：

```text
稳定来源资产类型 + 格式版本
    → 作者会话工厂
    → 默认保存命名／项目登记政策

会话种类 + 上下文需求
    → 可用内容视图工厂（可多个）
    → 绑定准备器 + 视图状态 codec
```

目标名可为 `AuthoringTypeRegistration` 和 `ContentViewRegistration`。实现前检查已有条目能否直接补充这些关系；不能同时保留一张新表和旧 `assetKind()` 人工表。

内置 Scene、Material、Flow 也登记同样关系。Application 只负责查询、歧义处理、固定版本、调用工厂和协调结果，不再识别各个 C++ Session 类型。

### 6.1 多重候选不能“第一个获胜”

同一来源有多个编辑器时：默认选择必须在描述或用户偏好中明确；没有唯一默认时返回候选，用户显式选择。不能依靠注册顺序、文件名排序或随意最高 priority 决定打开结果。

文件后缀是发现线索，不是唯一类型依据。持久 type、格式版本、codec／header 验证保持。TypeToken 仅用于同 ABI 内绑定检查，不写入文件。

### 6.2 来源格式与 runtime 资产类型

复用已有 `AssetTypeId`，不要新增 `EditorAssetTypeId` 表达同一件事。[S34]

但作者材质图与编译后 MaterialAsset 不是同一个格式；它们可以使用不同的规范类型名，描述关系明确连接。`SessionKindId` 则表达会话实现种类，也不与 AssetTypeId 混用。

### 6.3 Manifest 演进

当前五值 kind 不能容纳未提前列举的作者资产。[S04,S05]

本轮允许为开放类型增加一个明确的新 Manifest 版本：持久记录使用规范类型名、来源格式版本与必要的已有字段。旧 v1 只读解析为等价新内存表示；**仅在显式保存／发布时写新格式**，不得打开项目就覆盖旧文件。

稳定 hash 必须与规范名核对，不能默认不存在 hash 碰撞。未知但语法合法的类型记录保留并显示“缺提供者”；不把整个项目判成无效，不丢用户数据。无法理解的旧版本／非法字段按原严格策略拒绝，保留字节，不猜测。

旧布局、资产实际 codec 不因 C++ 类型搬家随意升级。没有实际格式必要的地方保持原版本。

### 6.4 派生产物

具体 MaterialCompiler、FlowCompiler、Skeleton 编码器保留各自领域输出。到共享文件发布边界时交付：拥有型字节／编码工作、AssetTypeId、来源戳、目标与编码版本。

不要在 Application 中增加 `SkeletonCompiled` 到 `VCompiledSource`。也不要抹掉所有编译类型变成裸 `any`。擦除只发生在真实异构保存／发布边界，持有 code 与完整 provenance。

## 7. 原有基础接口不足时允许真正重构

需要检查的不足包括：

- IViewHost 目前只有 describe 加 ViewRequests，完整 adopt／批量准备在具体 Host。选择继续暴露真实公开 ViewHost，或给原接口补所需最小操作；不能新建一个逐项转发 HostAdapter。
- 读取必须能够捕获 owning 端点，而不是要求插件持有整 ProjectStorage。若现有方法足够，则仅传相应端点。
- content-view 工厂须取得构造需要的公共会话能力，不能要求 Application 先构造每一种具体 Interaction。
- 关闭／重绑定涉及 payload 析构时，公开协议必须表达可重试与永久失败，而不是返回 bool 丢失事实。

细粒度不是无限分散；最小使用单元由真实职责决定。同一 provider 的紧密相关方法放在一起，普通算法不用虚接口。

## 8. ABI 与安装

这是公共插件契约变化，不允许在保持旧 V7 签名／结构大小的同时偷偷改变含义。根据最终变更更新 Editor 导出版本／ABI 指纹（预期需要下一版本，具体以已有机制确定），在调用前拒绝不匹配模块。runtime 插件 ABI 不因纯 Editor 改动无故变化。

公共头通过实际 provider 安装；不得对外发布 pinclude/sinclude 或整份 ApplicationImpl。内置与插件统一使用这些公开入口。旧 API 删除时同步所有源码、生成器、安装消费者，不保留 namespace alias、转发头和 Old/New 回退。


---

# 03　数据、执行者、结果与谓词语义

## 1. 不是成员函数和自由函数的审美争论

用户提出的关键问题是：谁是行为的承担者、处理什么数据、产生什么结果。`object.doSomething()` 不是唯一正确句法。

本轮采用三种并存的形态：

| 形态 | 适用对象 | 示例（目标表达） |
|---|---|---|
| 数据 + 算法／执行者 + 结果 | 输入与处理算法有独立生命周期／变化理由 | `compiler.compile(graph)` → compiled graph；`executor.undo(history)` → ApplyResult |
| 拥有资源的活动对象 | 本身负责准入、在途状态、句柄或资源 | `task_scope.submit(...)`、`view_host.adopt(...)`、`save_service.requestSave(...)` |
| 纯转换／判定函数 | 不需要独立状态和策略对象 | `planLayout(layout, observations)`、`encode(value)`、`checkCompatibility(...)` |

Graph 的 addNode 可以维护图本身结构；不必把它改成一个独立 NodeAdder。数据容器可有构造、查询、校验辅助和 RAII，不意味着它只能是 public POD。反之，命名为 Data 却跨线程执行闭包、采用 Session，就不是纯数据。

## 2. 当前 History 确切承担了什么

`EditHistory::Impl` 当前保存 entries、retired、cursor、StateId、revision、预算、owner 线程、phase 和 observer。`execute/replay` 不只是移动游标，而会：[S01–S03]

```text
校验身份／来源／预算
 → operation.prepare(context, budget)
 → plan.apply()
 → 改历史记录与 cursor/current
 → plan.publish(commit)
 → observer.changed
 → 退休项与 plan 清理
```

因此，不能假称“现有 History 已经只是数据”。用户提出的是对当前职责的重新划分。

## 3. 目标划分：EditHistory 与 EditExecutor

### 3.1 EditHistory

保留名字表示历史日志这一份数据，拥有：

- HistoryId、base/current StateId、revision、event 与已发号游标。
- 已接纳的操作／memento、标签、before/after、计量及 redo 分支。
- 当前 cursor、限额及实际分配的记录存储。
- 保证这份数据只能在原线程和合法阶段修改所需的**唯一内部控制元数据**。

公开只保留身份、只读 view／entry／snapshot，以及受控创建与析构。公开字段不得让外部直接修改 cursor 或 entries。

**纯数据职责不等于可复制、可随意移动或可持久化。** 条目可能含有绑定实际源和插件代码的 memento，仍应保持固定地址／线程与代码寿命约束。不能因为改叫日志，就序列化回调地址或拷贝一份可独立回放的历史。

### 3.2 EditExecutor

它执行原算法，不保存第二份 entries/current/revision。目标接口如下，仅为目标契约：

```cpp
class EditExecutor final
{
public:
    [[nodiscard]] EditResult<ApplyResult>
    execute(EditHistory& history, EditOperationPtr& operation) noexcept;

    [[nodiscard]] EditResult<ApplyResult> undo(EditHistory& history) noexcept;
    [[nodiscard]] EditResult<ApplyResult> redo(EditHistory& history) noexcept;
    [[nodiscard]] EditResult<void> clear(EditHistory& history) noexcept;
    [[nodiscard]] EditResult<void> close(EditHistory& history) noexcept;
};
```

它是原编辑算法的执行角色，不是 engine/process executor：不排线程、不管理 Task、不跨帧拥有一次撤销。可以无堆分配地构造；若实际实现没有状态，保留小算法对象或同命名空间函数均可，但项目内统一一种主要入口，不能两套算法。

选择成员形式不是因为“语法更先进”，而是用户要求清楚表达执行者。不得新增 IEditExecutor 或容器化 ExecutorRegistry，除非出现本轮没有提出的真实替代需求。

### 3.3 准入仍按 History 身份共享

两个 EditExecutor 实例访问同一 History，必须遇到同一保护。`phase` 不能只放在每个 executor 中，否则回调换一个 executor 就能绕过。

推荐保持 History 内一份不公开的 owner/phase/control 数据，只有 EditExecutor 修改。需要物理细分时可以在 private 中分 JournalStorage 与 ExecutionState，但不能多一个公开管理器或第二个忙标志。

这份控制元数据是数据一致性的一部分，不让 History 自行执行领域行为。观察通知由执行者在提交后发出；原 observer 的生命周期和通知失败契约保持，不能借本轮换成可能丢完成的广播。

### 3.4 原 Session 可以保留 undo()/redo()

Session 是工作副本的实际 owner，代表可执行编辑的主题。`session.undo()` 可以作为公开领域操作，内部持有／借用同一 History，并调用 `executor.undo(history)`。

这是聚合边界，不是为了迁移留下的兼容壳。底层 `history.undo()` 本身应删除；不能保留它承载算法，再加一个前置转发对象。

## 4. 精确迁移事项

| 当前项 | 处置 |
|---|---|
| EditHistory::execute/undo/redo/replay/clear/close | 算法迁到唯一 EditExecutor 实现，删除原变异声明和定义。 |
| EditHistory 的记录、游标、限额与查询 | 留在日志；不复制成 Executor 的“缓存”。 |
| PreparedEdit::friend class EditHistory | 调整为实际拥有 apply/publish 权限的 EditExecutor；不要公开 apply 给任意调用者。 |
| Entry / retired / PhaseGuard | 按唯一日志存储与受保护执行分配；尤其 plan、输入和 retired 清理要先于 guard 退出。 |
| SceneSession / MaterialSession / FlowSession 的调用 | 改用唯一执行者；读取、来源戳和会话准入顺序不变。 |
| Run 暂停编辑和其他 History 消费者 | S0 从真实引用展开后同批迁移；不能只改三模型而留下第二历史实现。 |
| History DLL 的身份计数 | 保留原跨 DSO 唯一域；不能挪到 header-local static 或每插件静态库。 |
| 安装头／包 | 物理路径和逻辑包不为这次语义变化全面改名；新增的必要头精确归原 provider，旧变异 API 不留 alias。 |
| 历史快照／旧验收 | 原字节不改；新活动测试用新 API 保留相同或更强行为。 |

原 `EditHistory::create()` 只构造有效日志，可以保留静态工厂；不为对称新增 HistoryBuilder、HistoryCompiler、HistoryManager。

## 5. 撤销失败与生命周期必须继续成立

撤销不是 `--cursor`。实施必须保留：

1. prepare 失败时作者源、游标、redo 和 memento 全部不变。
2. NO_CHANGE 不生成历史、不清 redo，但输入／plan 仍在正确 gate 内清理。
3. apply 的已准备提交与日志更新顺序不出现可观察半状态。
4. publish 发生于源与日志一致之后；回调重入、另一 executor 重入均被同一 owner 拒绝。
5. 截断、预算裁剪、clear、close 的条目析构在代码 pin 和保护范围内完成。
6. undo/redo、删除重建、分支不重用已公开 NodeId／PinId／对象身份。
7. 保存 checkpoint 不迁入 History；清历史与内容 dirty 的原语义不能被重新猜测。
8. close 通知与普通析构不混淆；不要求析构时重新进入用户回调。

不为了“History 数据不可变”而每次撤销复制整份日志。这里的不可随意修改是封装与准入，不是强制持久数据结构。

## 6. PreparedSessionData 的改名必须反映真实责任

当前它持有 Prepare/Reload 闭包，消费后在 owner 中创建或替换 Session。[S12] 本轮目标将其归为 `SessionPreparation`，保留已有 move-only、code owner、准入失败不消耗和一次消费规则。

worker 的解码输出可以是具体 `DecodedSkeleton`／既有 `SkeletonAsset` 拥有值；不同领域不必都继承一个 Data 接口。拥有型准备操作将这些值与实际 owner-stage 算法连接。

若沿原设计保留 `prepared.prepare(store, saves)`，名称必须清楚表示它是一次性准备操作。也可以将采用算法放现有安装执行者中；但不能为纯命名再加一层只转发的 SessionPreparationExecutor。

## 7. 快照与描述不是公共可变字段袋

### 7.1 ProjectCatalogSnapshot

目标为唯一私有不可变 Data 和只读方法：

```cpp
class ProjectCatalogSnapshot final
{
public:
    [[nodiscard]] ProjectCatalogVersion version() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::span<const AssetCatalogEntry> assets() const noexcept;
    [[nodiscard]] const AssetCatalogEntry* find(asset::AssetId id) const noexcept;
};
```

`find` 使用同一 Data 的索引，返回借用只在该快照 owner 存活时有效；需要跨任务持有则同时持快照或返回现有拥有型观察。不能调用 live Model 的 find 冒充冻结快照查询。

### 7.2 请求值与操作记录

`VResultIntent` 将动作与载荷绑定；`VWorkspaceIntent` 同理。所有小类型集中在一个相关内部语义头，不按类型数拆文件。

```cpp
struct AcknowledgeSave final { persistence::SaveId save; };
struct CancelSave final { persistence::SaveId save; };
struct ReconcilePublication final { persistence::WriteTicket ticket; };
using VResultIntent = std::variant<AcknowledgeSave, CancelSave, ReconcilePublication>;
```

上面是部分示意，不得因此删除现有其他结果动作。S0 从真实 switch 展开全部分支与载荷，再证明每个合法动作可表示、每个无意义组合不可构造。

在途操作则应保留为具有生命周期的类／明确状态组合。不能为了“数据纯化”，把 ArtifactPresentation 的资源和回调散成多个无 owner optional。

## 8. 谓词和命名清单

| 当前表达 | 问题种类 | 目标方向 |
|---|---|---|
| history.undo() | 数据名承担领域执行 | executor.undo(history)；session.undo() 可作为聚合入口。 |
| PreparedSessionData.prepare(...) | Data 名掩盖可执行／一次消费责任 | SessionPreparation；数据值与安装算法的边界明确。 |
| ResultIntent{action,target} | 动作与宾语能不匹配 | 动作专有载荷 variant。 |
| structure_request_=true/false | 布尔值隐藏添加／删除语义 | 明确 EComponentAction 或专有请求值。 |
| prepare/save/publish 都返回 bool | 可能混淆接纳与完成 | 原有结构化 Result／ticket 保留；只对实际存在的 bool 丢失事实处整改。 |
| capabilities 中的 is3D | 缺少主语和目标 | “目标对象支持 Spatial3D”“当前视口有 RayQuery3D”；不以名字推断。 |
| `snapshot.assets = ...` | 快照观察可脱离其 owner | 私有 storage + 只读访问器。 |

不得将每一个 `ensure/update/process` 按名称判错。先读实现：是查询、可能创建、推进工作还是提交事实，再按那份真实语义命名。

## 9. Concept 使用边界

Concept 约束真实算法所需表达式、返回类型和必要的 noexcept；不负责运行时目标适用性、代码寿命、线程和语义原子性。

优先保持具体数据和可静态组合算法；只在开放异构 owner／插件边界做一次擦除。不要模板化整个 Application 服务树，也不要用闭合 variant 冒充任意插件类型扩展。

可复用算法的输入从一开始就准确时，后续不需要每层重查同一事实。但跨 callback、IO、下一帧、代际变化后的复核依旧必要。


---

# 04　场景、对象、工具与渲染适用性

## 1. 本轮统一什么

统一的是“哪些事实作为依据、如何提出要求、如何给出可解释结果、何时失效”，不是创建一个全局 `SceneType` 或权限引擎。

事实继续来自现有 WorldDescription、schema、系统注册、FeatureDescriptor、运行状态和当前目标。底层不得为了 Editor 菜单引入 Pane、Command 或工作台类型。[S28–S33]

## 2. 三组事实不能混用

| 事实 | 来源 | 常见错误 |
|---|---|---|
| 某类型是否已安装 | 进程／项目注册快照 | 已加载 Transform3D 就认为所有 World 都允许 3D 组件。 |
| 某目标的持久内容契约 | World.schemas、实际对象、作者配置 | 只看全局注册，等用户点击后才由模型拒绝。 |
| 某操作此刻能否执行 | Session 准入、运行暂停、资源、目标代际 | 将临时 BUSY 当成不支持，或把不适用永久变成 BUSY。 |

相机透视／正交、对象空间维数、World 分区方式、运行对象查询后端与 Feature 输入要求各自独立。

一个 2D 工具不应因为 OrthographicCamera 存在就宣称满足；一个使用深度信息的效果也不一定只能由“3D 场景”使用。规则必须描述真实需求。

## 3. 最小公共模型

目标可以采用以下语义，具体名字在现有契约中归并：

- `AuthoringFacts`：对当前 Session／对象／配置和版本的只读观察。它不是第二份 World 状态。
- `ApplicabilityRequirements`：一项工具／组件操作／Feature 对已有能力的声明。可用现有稳定 type/capability 标识，不新造重复全局注册器。
- `ApplicabilityResult`：`SUPPORTED`、`NOT_APPLICABLE`、`TEMPORARILY_UNAVAILABLE`，附准确原因与来源版本。目标失效／读取失败是查询错误，不强行落入这三个成功结果。

规则和结果可以是小型纯值／纯函数；无状态查询不需要一个 ApplyManager。现有领域验证器继续负责最终合法性，公共查询应复用其纯判定部分，而不是复制相同逻辑两遍。

### 3.1 分层归属

| 内容 | 层 |
|---|---|
| 引擎本身的组件／系统／Feature 要求 | 原 engine/modules 描述层 |
| 作者数据允许的结构与 schema | authoring 对应领域 |
| 编辑模式、可准备性、跨操作条件 | activities 或 workbench 的对应策略 |
| 菜单隐藏／禁用／解释 | workbench |
| 注册关系和真实服务组合 | application |

没有任何反向边 `engine → editor`。若多个引擎消费者都需要新增的纯能力描述，可放原引擎契约中；仅 Editor 的展示偏好不应下沉。

## 4. 必须接入的四个消费者

### 4.1 组件添加

候选集合不能再等于 `schemas.all()`。至少结合：当前 World 声明、可创建作者值、非 runtime-derived、对象已有组件与互斥约束、当前编辑模式。

**已存在的不支持／未知组件不能被隐藏后丢弃。** 仍显示保留占位与诊断；需要用户明确迁移／移除时才改变数据。

最终 SceneSession.apply 继续核验。查询成功不是永久写许可。

### 4.2 对象与层级工具

Outliner 创建 2D/3D 对象必须依据当前目标允许的空间能力，不是注册表中是否有 Transform。

非层级 World 不应因为用户打开 Outliner 就被自动补 Parent。当前不支持的索引 World 编辑可以明确 `NOT_APPLICABLE` 或准确错误；本轮不要求实现所有分区结构编辑算法。

### 4.3 Render Feature

继续使用原 dependencies/conflicts/multiplicity/device profiles；不重写拓扑解析器。

只补目前缺少的输入／目标上下文关系，例如需要何种几何、资源、视图或场景能力。具体 Feature 作者声明需求；内置 Feature 也登记，不把“名字包含 shadow”当成三维判断。

预设只是方便的初始选择，不是合法性来源。更改预设后手工修改组件／系统，合法性仍由同一查询获得。

### 4.4 编辑器和工具

骨骼编辑器声明其支持的内容类型及所需能力，而不直接声明“只在3D显示”。如果实际骨骼格式支持不同维度，分别根据事实处理。

独立资产编辑器可以不依赖当前场景；场景附属工具应绑定明确 Scene/Session/Object，不读全局焦点作为隐含参数。

## 5. 查询、执行与失效

```text
捕获描述／内容／注册版本
    → 纯查询生成候选与原因
    → UI 呈现
    → 用户产生固定目标请求
    → owner 实际准入重新验证相关事实
    → 准备／提交
```

适用性缓存只保存派生结论，键至少包含它确实读取的来源域与 revision；配置变化、目标重绑、注册换代、运行模式变化分别失效。不能给每个 getter 新增一份缓存。

对不知道支持与否的未知扩展，不能默认 true；也不能因此删除历史数据。只读未知数据保留与允许新操作是两种政策。

## 6. 静态与动态规则

内置检查可使用普通函数或 concept 约束的规则；动态插件通过现有 owning contribution 传入只读检查，外层 containment 处理失败。

运行时 callback 必须禁止 IO、改作者内容、创建窗口及递归推进系统。查询异常／错误给出结构化失败，不当成“不可见”吞掉。规则执行也必须持定义代码的 lease。

不新增一个任意表达式 DSL、脚本解释器或全局 capability database。只有在重复的真实规则无法以现有描述表达时，才增加最小数据字段。

## 7. 验收组合

需要用实际模型覆盖：2D-only、3D-only、混合空间、无层级、已安装但未声明的 schema、未知保留数据、不同分区能力、资源暂不可用，以及注册版本更换。

同一规则在组件候选、对象创建、工具可用性和最终准入中应能解释一致结果，但不要求每个查询都返回一模一样的 UI 文案。

真实渲染效果涉及设备能力的部分用现有 GPU consumer；纯规则可无 GPU 验证。不能用 2D/3D 布尔单元测试代替当前 Feature 安装规则，也不需要重跑每一种渲染效果的完整视觉质量矩阵。


---

# 05　AGENTS.md 遵守情况与执行清单

## 1. 规范来源与本次核验范围

用户上传文件为 313 个实际文本行、CRLF。原字节 SHA-256：

`4ac02b300d99fd05ff7bcb5bced4bfc6f47b8e30caaa1a8821ace240e8f75357`

将 CRLF 仅在比对中规范化为 LF 后，Git blob 为 `1fd3123156dc7f59f98cbcd422f227a16352cddc`，与固定提交中 GitHub 返回的 AGENTS blob 一致。附件仍按原字节保存，不改写用户规范。

本次完整读取了用户提供的规范；代码只进行了记录在来源索引中的定向核查。没有完整源码 AST／全仓格式检查、独立引擎构建、生成器和安装运行，因此不能回答“所有条目均已遵守”。

## 2. 对上轮评价的校正

AGENTS 明确要求：复杂条件先分解为具名 `const bool` 语义组，最后判断聚合结果；外提不能破坏短路安全。

因此，“用了多个 is_* 布尔变量”本身不是违反可读性规范。不能为了缩短代码，把它们统一合并回长串比较。正确整改是：组名准确、独立校验分组清楚、无意义的同义中转减少，最终仍符合用户要求。

历史是否应是数据／执行者分离，是本次新增设计要求；AGENTS 并没有写“所有操作只能是成员函数”或“所有数据都不能有成员”。不能伪称该规范早已规定这项选择。

## 3. 当前能够直接指出的例子

| 条目 | 源码观察 | 判定 | 本轮处理 |
|---|---|---|---|
| 类型命名 | `Skeleton.hpp` 仍定义 `Bone_t`，注释承认历史概念重名。[S41] | 不符合当前 PascalCase 基线；这是存量规则问题，不是插件工厂故障。 | 改到该数据定义时准确正名并迁移实际消费者；不得保留同义 alias。 |
| 复杂判断分组 | DetachedView 构造中直接用 `!code.valid() || !pane || pane->attachedRoot() || pane->parent()`；EditorViews 容量条件也将不同校验直接组合。[S14,S06] | 与 AGENTS 的具名分组要求不一致；短路当前有保护，不能重排到先解引用空指针。 | 先 code／pane 存在，再有效时计算 attached／parent；聚合具名结果。 |
| 长返回类型／调用签名 | SceneConfigurationInputs 的嵌套 std::function、ViewStateResult 的长返回声明等。[S22,S14] | 存在应按 120 列及语义别名规则整理的实例；本次不报全仓行数。 | 用对应结果／回调的有意义别名，函数名另行；不把模板标识随意从 `<` 中间拆开。 |
| 只需描述却包含具体 View | Contributions.hpp 通过 InspectorView.hpp 得到 InspectorComponent。[S11,S19] | 公共依赖归属不够精简，亦违背 QR18 方向；并非简单删 include 就能编译。 | 迁出真实轻量描述定义，View 和贡献共同使用；禁止转发 shim。 |
| 可变快照破坏不变量 | ProjectCatalogSnapshot 的 public owner-dependent views。[S16] | 明确类型设计问题，主要对应 QR01/09/15；不是命名格式问题。 | 私有拥有数据 + 只读观察／索引。 |
| 动作和载荷分离 | ResultIntent / WorkspaceIntent。[S09] | 语义设计问题，对应 QR01/04；不能仅运行格式器修复。 | 有限请求 variant。 |
| 一般不使用异常 | InspectorView 的构造型 component 路径包含 catch bad_alloc 后 terminate，其他 catch 转 CODEC。[S18] | 与“允许边界捕获后立即转 Lux error”的文字存在需要裁定的分配失败政策差异；不能直接宣称全面合规。 | 在可恢复的 prepare/factory 边界返回准确错误；已进入无失败 commit 的 fatal 分配政策另行明确，不能假造回滚。不要全仓机械替换 terminate。 |

上表最后一项是**规范／既有失败政策的一致性问题**，不是已复现的功能 bug，也不是要求禁用 C++ 异常处理机制。

## 4. 逐类规范矩阵

| AGENTS 主题 | 当前依据／边界 | 实施方必须产生的结果 |
|---|---|---|
| editor/engine/modules 与五层 | 根结构和已读 provider 符合目标；完整链接未独立重跑。 | 真实 include/target/生成器/静态闭包；新增插件路径不反向依赖。 |
| 命名：类型、V alias、E enum、成员 | 代表性头多数符合；未做全仓 AST。 | 修改闭包内检查每个新/改类型；V 前缀仅对 variant alias，不误改 expected alias。 |
| 120 列、4 空格、括号 | 有直接待整理实例；未跑全仓 formatter。 | 按实际语义格式化改动文件，样例先人工核验，不生成全仓无关排版 diff。 |
| 复杂判断与短路 | 分组有正确正例，也有直接串联反例。 | 检查解引用、下标、整数运算的前提；阶段布尔量必须真的保护访问。 |
| include/sinclude/pinclude/src | 当前职责源已分层；精确安装依赖需构建核实。 | 同目录多 target 用实际 provider；不能对插件导出项目私有头。 |
| 不留兼容别名与自指 using | 已有阶段报告表明多处清除，不代表全仓为零。 | 新增替换 API 的全部调用和安装同步；历史参考不算活动 shim。 |
| 组件头不依赖行为层 | 本次没有遍历全组件头，不能认证。 | 对改到的组件头和它们生成依赖逐项查；公共编码约定归原 resource description。 |
| 公共头不拖重依赖 | 上述 Contribution/Inspector 是需要处理的依赖关系。 | compile/header closure 验证；需要完整 value/基类定义的不强行前置声明。 |
| 不主动 throw、热路径无 try/catch | 本次未作完整热路径分类；不能仅搜索关键字下结论。 | 区分业务 throw、STL/第三方边界 containment、测试代码；实际 dispatch/drain 中的捕获不能靠重命名 helper 隐藏。 |
| 错误与日志出口 | 原 structured errors/Result 继续复用。 | 不把错误一律转 BUSY，不让 render 链接 log，不新增 fprintf/cerr 宿主出口副本。 |
| assert 不承担 release 验证 | 本次未全仓检索；原 History 使用真实错误和 fatal 条件。 | 外部非法输入在 RelWithDebInfo 下真正拒绝；断言只用于内部已证明条件，不能代替运行返回。 |
| ECS observer 只记延迟命令 | 当前整改路径需特别保护增量投影。 | 观察中不直接改 Registry；排空期间新工作留后批，销毁读句柄后排队。 |
| observer 连接折入存量 | 未独立验证所有系统。 | 新改 observer 同时处理先建对象后连接；不能依赖构造顺序。 |
| on_update 用 patch/replace | 字段／增量投影修改时必须检查。 | 不用 get<T>().field=... 绕过通知；按原 schema/operation 接口执行。 |
| 异步就绪保留原轮询 | Process／GPU 的完成不是一次结构 signal。 | 不为统一 observer 而删除轮询；完成与资源装回世界在原安全点。 |
| all/-j4/-k0、两轮构建 | 历史日志不等于本轮已跑。 | 最终改动闭包完成后 all；CMake 改动第二轮 no work；构建与实机验证串行。 |
| modules 公共头三前缀同步 | 本轮若未改则不触发。 | 改到 modules 公共头时同步 Debug/RelWithDebInfo/Android include 并比对；Android 构建仍非默认任务。 |
| tracked clean 资格 | 容器无法获取完整 clone，不冒称执行。 | 实施方先 ValidateTrackedSnapshot，再固定 clean commit；不靠忽略文件补齐源码。 |
| QR01–QR22 和唯一账本 | 本包按原责任和记录位置。 | 只在原账本扩展 EC1；旧验收的结果和 SHA 不改写。 |

## 5. 异常规则的执行方式

不得主动用 throw 表达 Lux 的域错误。公开 runtime/domain 默认 noexcept 与 expected；`noexcept` 不自动说明“不分配”或“所有失败都可恢复”。

边界捕获必须有实际理由：Builder、Codec、fallible factory、toolchain 或外部插件调用。捕获后立即转换，不把异常传播跨 DLL、Task、Script、System 边界。

本轮不全局打开 `-fno-exceptions`／`/EHs-`。不向每层加入相同的 try/catch。清楚区分：

- 准备期正常可报告错误：返回结构化 Lux failure。
- 第三方异常：在最近的允许边界转换并保活错误载荷代码。
- 已声明为无普通失败的提交阶段：先通过准备消除可恢复失败；无法恢复的契约破坏按既有 fatal 政策处理，不返回“失败但已经改了一半”。

## 6. 机械检查和语义检查分开

可机械辅助：列宽／缩进、enum/variant 命名候选、私有头出现在安装列表、已删除 API 引用、throw/try/catch 候选、未经分类的调用依赖。

必须语义复核：短路安全、catch 是合法外部边界还是热路径补偿、某个 include 是否只为完整值定义、一个 bool 是否为动作状态、一个公共快照是否能被误构造。

不提供“扫描到 0 条字符串 = 完全遵守”的资格。误报和不适用项须在同一账本说明，不能简单关闭整个规则。


---

# 06　EC1 实施批次、删改清单与验收

## 1. 总执行规则

本轮只有一个 EC1 阶段，内部 S0–S7 按依赖推进；每批完成可构建的责任闭包，不能积累多个未完成实现再用兼容层粘接。

原 P13 已结束的事实保持。不得从旧数字推断“本轮必须至少多少项测试”。测试按真实行为映射，不能删除原断言只保名称，也不能为了数量固定保留已淘汰 API。

S0 必须核对当前实际 HEAD。若代码已经合入 main，使用授权开发位置和现行提交，不回退、不重建同名分支；本包本身不授权提交代码或删分支。

## S0：固定来源、事实和真正扩展范围

### 输入

当前源码、上传 AGENTS、docs/editor-quality、已有 P10Q 性能记录、最近 P12/P13 交付范围与用户补丁状态。

### 工作

- 记录 HEAD、分支／worktree、工具链、真实 lux-cxx/imgui/node-editor/toolset 安装版本；不用本包旧依赖 SHA 覆盖较新已验证依赖。
- 从真实 Git 文件与调用符号生成 EC1 处置表：路径、符号、实际 target、public/sinclude/pinclude、消费者、动作和替代位置。
- 将 O01–O09、D01–D09、P01–P06 与 AGENTS 表逐项复核；本次未知项保持 UNKNOWN，不能通过照抄变成已证实。
- 列出开放轴：作者资产／会话／内容视图／可选导入与派生产物／场景适用性。固定阶段枚举、wire 标签和旧数据映射不列入开放整改。
- 核对已有 Skeleton 数据与 codec、参考骨骼编辑动作；不创建重复的 asset 或序列化规则。
- 保存旧工厂、反射、History、保存、Run、GPU、输入及用户免验的行为映射。

### 出口

所有计划改动都有实际 provider；没有“添加一个接口以后再找消费者”的空项目。记录缺失资产 registry 等实际差距，不发明它已经存在。

## S1：数据／执行语义和类型不变量

### 必做

1. 按第 03 文档迁出 EditHistory 的执行算法，新增最小 EditExecutor，原日志保持唯一。
2. 更新 PreparedEdit 权限、各 Session、运行暂停编辑、历史测试及公共安装消费者。
3. 将 PreparedSessionData 的可执行准备语义正名并更新所有生产调用；无转发 alias。
4. ProjectCatalogSnapshot 私有化 owner-dependent 观察，加同快照查询。
5. ResultIntent、WorkspaceIntent 与 Inspector 布尔动作按真实合法状态收敛。

### 明确删除

EditHistory 的旧变异成员声明与定义；旧调用；PreparedSessionData 的同义兼容名；快照公开可写视图；action 与不匹配 target 的手工配对；不再使用的错误分支。

### 必须验证

原所有 History 行为，包括 prepare 失败、NO_CHANGE、预算、裁剪、旧 redo、两 executor 重入、publish/清理回调、code pin、关闭通知和跨 DSO 身份唯一性。测试必须真的经过新执行者，不能保留一份旧算法作运行 oracle。

在 freeze/description 类改动时不为了“纯数据”解除 RAII、线程或借用限制。

## S2：正式基础能力与插件激活

### 必做

- 给外部插件提供可独立使用的资产目录／冻结读取、会话、历史执行、Pane 构造、Host 请求、命令、Process、发布等正式能力。
- 每项优先复用原 public provider；不足时在原 owner 补窄接口或提取真实职责。
- 建立现有扩展装配中的显式能力注入、activation 生命周期和依赖版本协商；不能把 ApplicationImpl 指针伪装成服务。
- 让一个内置工具先迁到这些能力，证明不是“外部 SDK 另一套包装”。
- 将公共描述从具体 View 头中剥离，依赖按真实层放置。

### 出口

可构建三种安装消费者：只读资产的无 UI 工具；只创建／登记 Pane 的独立窗口；无 UI 的真实会话编辑与保存。它们不需要链接整个 EditorApplication，不借源码私有头。

插件仅需 CPU 能力时不被强制要求 renderer／LLVM；确实请求 GPU／编译时显式链接。

## S3：开放内容路由与完整内置迁移

### 必做

- 采用第 02 文档中的稳定来源类型、格式与会话／视图关系。
- 演进 Manifest 以容纳开放类型；v1 读取、未知类型保留、新格式显式发布和中断安全一起实现。
- 删除 Application 中打开、保存、后缀、视图组合和恢复的三类硬编码。
- 内置 Scene/Material/Flow 也通过同样的关系和能力激活；不保留快速特例作为业务后门。
- 派生产物发布移出闭合 VCompiledSource 产品表，复用领域 compiler 和公共发布边界。
- 内容—视图关联支持零／一／多内容引用，并保留固定目标、关闭与恢复规则。

### 遇到缺 provider 的行为

缺格式 provider：保留目录记录与原字节，报告不可打开。
缺视图 provider：内容可以保持已发布且可查询，显示失败不得回滚合法内容。
缺保存角色：明确 UNAVAILABLE，不假装 clean。
恢复找不到 exact ViewType：保留记录，不随意改用同种内容第一个窗口。

### 出口

所有三类内置内容通过通用路径完成打开、Save As、重载、SaveAll、关闭和恢复；源码中没有要为第四类内容再修改的重复路由表。

不要求与开放性无关的旧格式标签 switch 消失。

## S4：统一适用性

### 必做

- 从原描述形成版本化事实观察；组件、对象创建、Feature 和工具共用需要的纯判定。
- 区分不适用、暂不可用、无效目标／读取失败。
- 内置规则与插件规则相同接入；各领域仍拥有自己的合法性实现。
- 删除 Inspector 的无条件全 schema 候选和 Outliner 只按全局 Transform 注册启用的旁路。
- 保留未知已存数据；场景能力不足不等于可以删除 payload。

### 出口

2D／3D／混合／无层级／已安装未声明／已移除插件／不同分区契约均有行为测试。改变一个事实源后，各相关消费者得到一致可解释结论；不要求统一 UI 文案。

## S5：针对性性能与检查收敛

### 默认必做

- Outliner 的稳定身份折叠状态和 O(C×N) 查询整改。
- 同一 ProjectCatalogSnapshot 的 by-id 索引，ModelCreation 依赖核验不逐 primitive 扫全目录。
- 删掉已被构造契约证明且中间没有失效边界的重复 lookup/describe；每处附有效期说明。
- 新路由按注册／项目 revision 预建不可变查询，不在每帧调用全部插件工厂或遍历全项目。

### 测量后裁定

- SceneSource 的 N×S 扫描：以代表性实际 schema／组件分布观察；能用现有池／成员关系改为实际存在组件遍历时整改。不能在没有修改收益时新建同步困难的全局索引。
- SceneProjection：记录至少“无变化、普通字段、结构、配置”的成本。优先避免与显示无关的更新；字段增量仅在原组件 API、observer 和退休协议可保持时采用。完整结构／未知变更仍有准确 fallback。
- copyOpaque／freeze：只共享真实不可变拥有块，不把调用者可改的 shared_ptr<const T> 当冻结保证。

**裁定不意味着可以不做任何调查。** 必须给出对应真实调用、规模和算法／分配计数；若保留原实现，说明成本与理由，不写“已优化”。

### 计时限制

不补旧 50k 慢算法样本；不固定要求每项 100 次。用短代表性样本、操作计数、必要的分配和 inclusive/exclusive 观察回答问题。构建与实机采样不并行，计时和插桩计数分开。

“栈浅”不是出口；在优化构建里观察真正间接调用、重复数据处理和耗时，而不是统计源码方法数。

## S6：外部骨骼编辑器验证

### 骨骼类型不是新造的

复用 `lux::rdesc::Skeleton`、`SkeletonAsset`、其 AssetTypeId 和 TAssetSerDeser。[S35,S41] 作者 Session 可以是插件的新领域工作副本；运行 Skeleton 数据和 codec 不复制。

最小真实能力：读取一个小型合法骨骼、显示已有骨骼条目、修改名称及至少一个受控且可验证的已有属性（如 global_transform）、Undo/Redo、冻结、保存与重开。不要用只有一颗整数的 FakeSession 冒充骨骼编辑器。

骨骼父顺序与 mesh 索引引用必须正确。若展示 reparent／重新排序，需要保证依赖引用和 bind/inverse-bind 约定；本轮不以此为必选功能，禁止为完成验收偷偷破坏下游 mesh。

### 同一套 API 必须通过的三条路线

| 路线 | 验证 |
|---|---|
| 低层无 UI | 插件只通过安装 SDK 查询／读取资产，创建会话，执行历史，保存和重读。 |
| 自由 Pane | 插件直接组合原 Pane/Element，登记、挂载、描述、焦点、关闭；不要求成为某个固定 IEditor。 |
| 完整内容工作流 | 项目登记、双击打开、两个视图共享同一会话、保存／Save As／重载／关闭／恢复；宿主不含 Skeleton 特判。 |

可以用同一个插件和同一个测试程序的不同模式验证，不必建立三个 SDK 系统。

### 强负例

- 注册了新类型却忘记视图：内容留存，显示失败准确。
- 同一格式两个默认编辑器：返回歧义，不按注册顺序决定。
- 插件缺失后项目重开：记录及字节保留，已有其他资产仍可使用。
- 外线程、回调内关闭、读取中重载、旧 Session/View 代际：拒绝路径不改源与 owner。
- 在任务／窗口／错误载荷仍存活时请求卸载：明确延迟／拒绝，不提前释放代码。
- 只安装 SDK：不能 include ApplicationImpl、pinclude/sinclude 或借旧 build DLL。
- 查看宿主 diff：除通用关系／能力的实现外，不能有 skeleton 类型名、枚举分支或专用 getSkeletonService。

验收示例可以位于独立 consumer 工程，但必须通过真正 DLL／版本协商使用通用宿主。不能直接把测试 TU 链入宿主伪装成插件。

## S7：规范、删除和最终交付

### 删除交叉清单

| 项目 | 必须清除／替代 |
|---|---|
| History | 旧执行成员原体、旧权限、旧消费者；不得有双算法。 |
| 资产路由 | 生产路径中的旧 closed kind 三类表；v1 只读迁移表保留。 |
| 视图绑定 | Application 对每工具 Interaction/Preview 的硬编码；通用关系替代。 |
| PreparedSessionData | 旧名与 API 迁移残留，不留转发别名。 |
| 快照与请求 | 可改 owner-dependent fields、动作与无关 target 配对。 |
| 贡献描述 | 为取得描述而依赖完整具体 View 的公共头关系。 |
| 适用性 | 与统一查询并行的全 schema／全 Feature UI 旁路。 |
| 复杂度 | 被新索引替代的重复线性扫描和死成员；不能只新增缓存不删旧扫描。 |
| 安装与代码生成 | 旧头／包 alias／生成路径／导出版本、消费者全部同步。 |

### 最终资格

- 按 AGENTS 做 tracked snapshot 检查、固定 clean commit、全量 `all -j 4 -- -k 0`；CMake 改动后第二轮无新增工作。
- 原 History、三作者模型、P05/P11 R1、Run、Host、读写／文件冲突、实例代际与代码寿命的受影响行为保持。
- CPU/PLAYER 隔离和实际禁止依赖正反夹具；新公共头按 provider 独立安装／编译。
- 真实骨骼 DLL 的三路线；三种内置编辑器通过同一开放路由的回归。
- 与变更相关的既有双视口／输入／GPU 资源路径，不把 mock 当 GPU；不重启无关长测。
- Linux、系统 IME、已免验旧人工链和旧性能 PARTIAL 原样保留；不擅自变成通过。

### 24 个验收主题（不是测试程序数量）

| ID | 观察 |
|---|---|
| XEC-01 | 新数据／执行边界真实，旧 History 变异算法已迁出。 |
| XEC-02 | Undo/Redo/NO_CHANGE/预算失败保源、保日志、保 redo。 |
| XEC-03 | 不同执行者对同历史重入仍拒绝；外线程错误准确。 |
| XEC-04 | 输入、plan、retired、闭包、错误与代码在正确顺序释放。 |
| XEC-05 | 无 UI 的资产查询与冻结读取 consumer。 |
| XEC-06 | 原 Pane 的独立构造、登记、挂载与关闭 consumer。 |
| XEC-07 | 内置与插件使用相同的能力注入，无私有业务后门。 |
| XEC-08 | 开放类型 Manifest、旧格式读取、未知 provider 保留。 |
| XEC-09 | 路由发现、多个编辑器和歧义处理。 |
| XEC-10 | 通用打开、保存、重载、SaveAll、关闭和恢复。 |
| XEC-11 | 内容发布成功但显示失败的部分完成。 |
| XEC-12 | 一内容多视图、复合视图多内容、独立工具无内容。 |
| XEC-13 | 骨骼实际字段、原 codec、Undo/Redo、重开字节语义。 |
| XEC-14 | 插件缺失／撤销、任务与窗口存活、真实 DLL 寿命。 |
| XEC-15 | 2D/3D/混合/无层级的候选与最终准入。 |
| XEC-16 | 已加载但未声明组件、未知既存数据保留。 |
| XEC-17 | Feature 依赖/冲突/设备需求仍用原规则。 |
| XEC-18 | 能力查询的版本失效与固定目标，不读全局焦点。 |
| XEC-19 | 快照不可破坏 owner 关系，同快照索引正确。 |
| XEC-20 | 无效动作／载荷不可构造，所有原合法动作仍可表示。 |
| XEC-21 | Outliner／模型目录核验的操作计数不再相乘增长。 |
| XEC-22 | 快照／投影成本和检查有效期有准确记录；无虚构加速。 |
| XEC-23 | AGENTS 修改闭包检查、独立头、真实依赖和生成同步。 |
| XEC-24 | 同一最终提交的行为映射、删除结果、支持／免验范围。 |

## 2. 改动候选路径（必须由 S0 展开真实调用）

```text
editor/editing/include/.../EditHistory.hpp
editor/editing/include/.../EditOperation.hpp
editor/editing/src/history/EditHistory.cpp
editor/authoring/{scene,material,flow}/...Session*
editor/authoring/project/{ProjectManifest,ProjectCatalogModel} 对应头与 CPP
editor/activities/project/ 的源读取、项目登记、导入与发布
editor/activities/sessions/ 的工厂、安装与内容操作
editor/activities/persistence/ 的真实保存／发布入口
editor/activities/scene/ModelCreationOperation 与投影活动
editor/workbench/desktop/ 的窗口工厂与现有 Host
editor/workbench/{scene,material,flow,project}/ 的消费者
editor/application/{EditorViews,EditorSaving,EditorRecovery,EditorArtifacts} 等组合
editor/application/extensions/ 的导出、贡献与内置登记
engine/modules 的实际需要补充的纯描述和 Skeleton 命名消费者
cmake 的实际组件、安装、SDK consumer 和架构规则
```

省略号表示从源码展开，不表示可以创造不存在的路径。新类型归现有主题；除必要独立 consumer 外不创建新顶层。

## 3. 阶段交付格式

每批交接说明：实现 SHA、实际工作区、用户补丁状态、已迁移责任、已删除旧项、尚未迁移的真实消费者、测试命令和结果、未测／免验范围。

最终实现与验收分别提交。报告结论按以下区分：源码已实现、静态风险已修、真实 SDK 已验证、继承证据、未运行、用户免验。没有运行的外部骨骼完整路径不能写成“插件扩展完成”。

不要再生成多套重复证明脚本。现有 stage/STRICT 检查器扩展 EC1 授权和依赖规则，保证旧阶段仍按旧 SHA 核验；不能把 `P13` 的标签继续用作 EC1 新代码的通过凭据。

## 4. 结束而不继续膨胀

EC1 完成后，架构应当更少认识内置具体类型，用户得到更多可单独使用的正式能力，而不是更多抽象层。

没有明确用户功能需求时，不继续追加：任意 Graph 插件序列化、全部投影增量、任意热卸载、完整动画工作台、无限可配置 capability DSL 或全平台 CI。发现真实正确性问题必须说明；纯粹“还能更抽象”不构成新增阶段。


---

# 07　源码、规范与证据范围

固定仓库：`LUX-YU/lux-engine`；提交：`54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8`。

## 1. 当前源码

“本次读取”表示在本轮调用 GitHub connector 取得内容；“固定 SHA 上下文中已读”表示同一 SHA 的实际文件内容已经出现在连续审阅上下文，本轮未重复下载。未标完整的文件只支持所列符号／片段，不据此宣称全文件审计。

| ID | 路径和正式来源 | 支持的范围 | 读取状态 |
|---|---|---|---|
| S01 | [`editor/editing/include/lux/engine/editor/editing/EditHistory.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/editing/include/lux/engine/editor/editing/EditHistory.hpp) | EditHistory public mutations | 本次读取 |
| S02 | [`editor/editing/src/history/EditHistory.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/editing/src/history/EditHistory.cpp) | Impl; execute; replay; clear; close | 本次读取完整算法段 |
| S03 | [`editor/editing/include/lux/engine/editor/editing/EditOperation.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/editing/include/lux/engine/editor/editing/EditOperation.hpp) | EditOperation; PreparedEdit; friend boundary | 本次读取 |
| S04 | [`editor/authoring/project/include/lux/engine/editor/project/ProjectManifest.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/authoring/project/include/lux/engine/editor/project/ProjectManifest.hpp) | EProjectAssetKind; ProjectAssetEntry | 固定 SHA 上下文中已读 |
| S05 | [`editor/authoring/project/src/ProjectManifest.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/authoring/project/src/ProjectManifest.cpp) | kinds; validation/codec helpers | 本次读取 1–235 行 |
| S06 | [`editor/application/src/EditorViews.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/src/EditorViews.cpp) | openCaptured; makeContentView; wireContentView | 固定 SHA 上下文中已读 |
| S07 | [`editor/application/src/EditorSaving.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/src/EditorSaving.cpp) | assetKind; prepareSave; askSave; cancelContentPreview | 固定 SHA 上下文中已读 |
| S08 | [`editor/application/src/EditorRecovery.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/src/EditorRecovery.cpp) | recoveryType; settleRecovery; captureRecovery | 本次读取 1–220 行 |
| S09 | [`editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp) | ContentView; VCompiledSource; ResultIntent; WorkspaceIntent | 固定 SHA 上下文中已读 |
| S10 | [`editor/application/extensions/include/lux/engine/editor/extensions/EditorExtension.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/extensions/include/lux/engine/editor/extensions/EditorExtension.hpp) | V7 exports; contribute(Draft&, CodeLease) | 本次读取 |
| S11 | [`editor/application/extensions/include/lux/engine/editor/extensions/Contributions.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/extensions/include/lux/engine/editor/extensions/Contributions.hpp) | ContributionDraft; ContributionSnapshot; ContributionRegistry | 固定 SHA 上下文中已读 |
| S12 | [`editor/activities/sessions/include/lux/engine/editor/sessions/SessionFactory.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/activities/sessions/include/lux/engine/editor/sessions/SessionFactory.hpp) | PreparedSessionData; SessionFactoryEntry; SessionLoadJob | 固定 SHA 上下文中已读 |
| S13 | [`editor/workbench/desktop/include/lux/engine/editor/views/ViewFactory.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/desktop/include/lux/engine/editor/views/ViewFactory.hpp) | ViewFactoryDescriptor; ViewFactoryInput; ViewFactorySnapshot | 固定 SHA 上下文中已读 |
| S14 | [`editor/workbench/desktop/include/lux/engine/editor/views/IViewHost.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/desktop/include/lux/engine/editor/views/IViewHost.hpp) | DetachedView; PreparedViewState; ViewRequests; IViewHost | 本次读取 |
| S15 | [`editor/activities/project/include/lux/engine/editor/storage/ProjectStorage.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/activities/project/include/lux/engine/editor/storage/ProjectStorage.hpp) | captureSource; captureAssetReads; assets; preparePublication | 本次读取 |
| S16 | [`editor/authoring/project/include/lux/engine/editor/project/ProjectCatalogModel.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/authoring/project/include/lux/engine/editor/project/ProjectCatalogModel.hpp) | ProjectCatalogSnapshot; ProjectCatalogModel | 固定 SHA 上下文中已读 |
| S17 | [`editor/activities/tasks/include/lux/engine/editor/tasks/TaskMonitor.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/activities/tasks/include/lux/engine/editor/tasks/TaskMonitor.hpp) | Snapshot; snapshot; requestCancel; observer lifetime | 本次读取 |
| S18 | [`editor/workbench/scene/src/InspectorView.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/src/InspectorView.cpp) | options; changeComponent; clear; rebind | 固定 SHA 上下文中已读 |
| S19 | [`editor/workbench/scene/include/lux/engine/editor/scene/InspectorView.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/include/lux/engine/editor/scene/InspectorView.hpp) | InspectorComponent colocated with concrete view | 固定 SHA 上下文中已读 |
| S20 | [`editor/workbench/scene/src/OutlinerView.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/src/OutlinerView.cpp) | readRows; update; draw; reparent | 固定 SHA 上下文中已读 |
| S21 | [`editor/workbench/scene/src/SceneConfigurationElement.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/src/SceneConfigurationElement.cpp) | FeatureField; system config; encode; presetFeatures | 固定 SHA 上下文中已读 |
| S22 | [`editor/workbench/scene/include/lux/engine/editor/scene/SceneConfigurationElement.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/include/lux/engine/editor/scene/SceneConfigurationElement.hpp) | SceneConfigurationInputs; ConfigurationControl | 固定 SHA 上下文中已读 |
| S23 | [`editor/authoring/scene/src/SceneSource.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/authoring/scene/src/SceneSource.cpp) | objects; validate; build; capture; copyOpaque | 固定 SHA 上下文中已读 |
| S24 | [`editor/activities/scene/src/SceneProjection.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/activities/scene/src/SceneProjection.cpp) | update; replace; instantiateAuthorProjection | 固定 SHA 上下文中已读 |
| S25 | [`editor/activities/scene/src/ModelCreationOperation.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/activities/scene/src/ModelCreationOperation.cpp) | commit dependency scan; captured catalog; codec recheck | 固定 SHA 上下文中已读 |
| S26 | [`editor/workbench/flow/src/FlowView.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/flow/src/FlowView.cpp) | NodePropertiesDraft; CanvasRequest; shared delivery | 固定 SHA 上下文中已读 |
| S27 | [`editor/workbench/material/src/MaterialView.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/material/src/MaterialView.cpp) | Properties; node/settings UI; delivery | 固定 SHA 上下文中已读 |
| S28 | [`engine/domain/world/description/include/lux/engine/world/WorldDescription.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/engine/domain/world/description/include/lux/engine/world/WorldDescription.hpp) | schemas; partitioner; partitionIndexes | 固定 SHA 上下文中已读 |
| S29 | [`engine/domain/world/README.md`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/engine/domain/world/README.md) | persistent identity; dimensions; declared design limitations | 固定 SHA 上下文中已读；设计文字不视为实现证明 |
| S30 | [`engine/domain/spatial/README.md`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/engine/domain/spatial/README.md) | partition index versus active-entity queries | 固定 SHA 上下文中已读；设计文字不视为实现证明 |
| S31 | [`engine/domain/simulation/ecs/schema/include/lux/engine/simulation/ecs/ComponentSchema.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/engine/domain/simulation/ecs/schema/include/lux/engine/simulation/ecs/ComponentSchema.hpp) | author construction; snapshot and semantic kinds | 固定 SHA 上下文中已读 |
| S32 | [`modules/function/render/client/include/lux/engine/function/render/client/core/RenderFeatureRegistration.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/modules/function/render/client/include/lux/engine/function/render/client/core/RenderFeatureRegistration.hpp) | scene_configurable; codec | 固定 SHA 上下文中已读 |
| S33 | [`modules/function/render/client/include/lux/engine/function/render/client/core/FeatureDescriptor.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/modules/function/render/client/include/lux/engine/function/render/client/core/FeatureDescriptor.hpp) | dependencies; conflicts; multiplicity; device profiles | 固定 SHA 上下文中已读 |
| S34 | [`modules/resource/asset/include/lux/engine/resource/asset/AssetTypeId.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/modules/resource/asset/include/lux/engine/resource/asset/AssetTypeId.hpp) | stable type token from canonical name | 本次读取 |
| S35 | [`modules/resource/asset/include/lux/engine/resource/asset/animation/SkeletonAsset.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/modules/resource/asset/include/lux/engine/resource/asset/animation/SkeletonAsset.hpp) | existing SkeletonAsset; lux::rdesc::Skeleton; codec | 本次读取 |
| S36 | [`docs/editor-quality.md`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/docs/editor-quality.md) | QR01–QR22 | 本次读取全部 QR01–QR22；末尾继承表不作为新证据 |
| S37 | [`AGENTS.md`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/AGENTS.md) | repository engineering baseline | 本次读取前 145 行；上传完整文件 LF 规范化 Git blob 与远端一致 |
| S38 | [`editor/application/src/EditorArtifacts.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/src/EditorArtifacts.cpp) | artifact product composition | 本轮仅沿用已读类型与调用背景；具体全函数需 S0 再核对 |
| S39 | [`editor/application/src/EditorSceneTools.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/src/EditorSceneTools.cpp) | showSceneTool; runtime/author distinction | 固定 SHA 上下文中已读 |
| S40 | [`editor/workbench/scene/src/SceneView.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/src/SceneView.cpp) | model drop; projection; camera/picking | 固定 SHA 上下文中已读 |
| S41 | [`modules/resource/description/include/lux/engine/description/Skeleton.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/modules/resource/description/include/lux/engine/description/Skeleton.hpp) | Bone_t; Skeleton; parent ordering; bind transforms | 本次读取 |

## 2. 用户来源

`references/AGENTS.user.md` 是用户上传规范的原字节。哈希：`4ac02b300d99fd05ff7bcb5bced4bfc6f47b8e30caaa1a8821ace240e8f75357`。CRLF→LF 只用于比较；规范化 Git blob 等于远端 AGENTS，未改写附件。

`references/P10Q.performance.historical.md` 是已有 P10Q 专项性能记录（如果本包包含）；它绑定 `3c20910d…`，不是当前最终产品或 EC1 的新实测。原 66/100、负载与分配覆盖范围保持。

## 3. 外部语言参考（补充，不替代项目规范）

- C++ Core Guidelines C.2/C.3/C.4：类的不变量、接口与实现、普通函数／成员函数的选择。https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
- C++ 工作草案的模板约束：concept 约束表达式与语法可用性，不自动证明运行期业务语义。https://eel.is/c++draft/temp.constr

项目的命名、排版、异常、构建和阶段政策以用户 AGENTS 为准。外部参考没有被用来推翻具名谓词分组规则。

## 4. 本轮没有做的事情

没有修改仓库，没有完整 clone/AST/链接闭包审计，没有独立运行引擎、真实 SDK、DLL、GPU、IME 或性能基准，没有复算历史归档全部哈希。容器直接网络读取不可用；仓库研究通过 GitHub connector 完成。

新增问题按代码事实／设计判断／静态风险／待核对分类。本包不是已验收补丁，不以文档完整性自检代替工程验证。

## 5. 本地交付自检范围

仅校验文档文件存在、相对链接、来源编号、JSON、用户规范原字节及 ZIP 可读取性。正文中的新 API 为目标签名，不宣称已在 Lux SDK 编译。
