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
