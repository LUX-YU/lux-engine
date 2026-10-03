# EC2 完整施工总册

职责重分配与游戏脚本能力收敛。当前基线 `248adc4576943cab83976afd8d1d5f31b63b70a9`。

单册与分册正文相同；链接可在完整包内访问。所有目标 API 以施工说明为准，不构成已实现声明。


---

<!-- source: 00_MASTER.md -->

# EC2 施工总指令：职责重分配与游戏脚本能力收敛

**日期：2026-10-03**  
**仓库：LUX-YU/lux-engine**  
**分支：codex/editor-redesign-v4**  
**当前输入 HEAD：`248adc4576943cab83976afd8d1d5f31b63b70a9`**  
**EC1 实现：`137b8f441faa1dbb7dfa792b6f01b513327e6248`**  
**性质：面向实施 LLM 的新阶段施工文件；不是 EC1 独立验收记录，也不是已经实施的新代码。**

## 1. 本阶段要达到什么

在已经完成的 EC1 上继续收敛，完成两条相互关联、但不得形成反向依赖的工作：

1. 将剩余混合职责拆回实际 owner：发布计划与占用权、配置草稿与 UI、产物发布与 Application、预览配方与预览资源、存储与迁移政策。
2. 使游戏脚本调用真实游戏能力。本轮完整打通运行期资产读取、结果观察与释放，并以既有时间／组件接口组成真实运行场景；不是向 Lua 暴露 Editor 的打开窗口、撤销、布局和项目发布。

共同标准：数据的含义准确；执行者处理明确输入；权限和资源寿命可追踪；C++、内置工具、插件与语言投影复用原提供者，而不是复制业务算法。

本阶段不是重开 P00–P13，也不重新排列五层。不得因引入游戏脚本，把 `editor/activities` 整体搬到 engine。Editor 的工作副本、项目作者格式和发布政策仍有独立产品语义。

## 2. 材料优先级与历史处理

优先级为：用户本次要求 → 当前 AGENTS 与持续质量规范 → 本施工包的明确新裁定 → 既有已验证行为 → 旧职责调查的建议。

- 原职责调查绑定 `54f8a563…`，其中“并入 EC1”已被本次 **EC2** 替代；原文件不修改。
- EC1 已报告完成 History、开放内容路由、V8 激活、骨骼三路线和若干类型／性能收敛。先继承并核对，不重做。
- 不以旧调查中的过时符号证明当前仍有缺陷：`EProjectAssetKind`、`VCompiledSource`、`PreparedSessionData`、旧 History 变异方法已报告退出。[C03]
- 本次已核对新 HEAD、实现父提交、EC1 报告及代表性源码；没有独立重跑 EC1 矩阵。R0 应核验前置材料与真实调用，不得把本文件当成新的 EC1 PASS 收据。
- 需要修订 ABI、真实数据格式或已公布语义时，写清理由与消费者迁移；禁止静默修改旧格式解释。绝不为了少代码破坏未知数据保留。

## 3. 范围：必做、条件做、不做

| 分类 | 内容 |
|---|---|
| 必做：职责 | ProjectPublication 封装计划／权限；配置草稿与准备算法无 UI 化；派生发布回到实际活动；两个通用面板不再借 AppImpl；预览输入键与目标分域；Workspace 查询／IO／纯转换分离；模型 recipe 提取与命名收敛。 |
| 必做：脚本 | 资产基础链的 C++／Lua 共用原读取与 codec；明确类型、能力注入、实例寿命、异步结果、错误、预算、停止／释放；PLAYER 和 Editor Run 同一运行期接线。 |
| 必做：架构 | 实际依赖和 owner 图；安装边界；同一路径的可测试性；旧入口删除；AGENTS 逐条处理修改闭包。 |
| 条件做 | 现有 ScriptAbility 实例作用域或资源结果清理不足时，补原准备／生命周期的窄钩子；已有等价能力则复用。没有真实不足不修改通用 VM 或 ABI。 |
| 条件做 | 准备/索引/编码的可测重复成本；只优化修改链，不启动全引擎性能运动。 |
| 本轮不做 | Lua 文本 IDE、断点调试、热重载保留任意运行状态、全游戏 API 自动绑定、网络/音频/物理全套新功能、IK/动画编辑器、完整增量投影、任意插件热卸载、新线程池或脚本执行器。 |

“接口可供脚本封装”是设计要求；“全部现有游戏功能已经绑定”不是本轮隐含承诺。R0 必须给各能力标出 **本轮实现／复用验证／预留且未实现**，不得以一份未来表宣称全部完成。

## 4. 禁止的捷径

- 不能用 `LuaEditorContext` 或给脚本一个 `ProjectActivities` 来解决资产读取。
- 不能令 PLAYER 链接 Editor 的 Session、History、ProjectStorage、SaveService、Pane、ImGui、作者 codec 或编辑器 LLVM 工具链。
- 不能让 Lua worker 直接 `lua_resume`，不能让游戏脚本在 Hook 内同步等磁盘/Task/Renderer。
- 不能将 `SharedBytes`、`shared_ptr<Asset>`、`std::span`、C++ 引用或裸指针 memcpy 到现有外部异步结果槽。
- 不能仿冒 timer 的 method 名称，取得仅由 ScriptTimers 铸造的本地异步资格。[C14]
- 不能为职责分配复制发布状态机、任务表、资产缓存、注册器、History 或 Current/Dirty。
- 不能将 Owner 拆成外部可改字段，然后要求所有消费者靠纪律保证正确。
- 不能只新建 Processor，其方法转发到仍含原全部逻辑的旧方法。
- 不能留下转发别名、Old/New 开关或永久兼容壳；数据只读迁移不属于该删除范围。
- 不能将 `supported()`（能力存在）当作 `admitted()`（当前操作准入），更不能将已接纳当作已完成。

## 5. 阶段组织

使用唯一 `.internal/editor-redesign/` 账本的 `ec2` 节点，不另建平行可变记录系统。

| 批次 | 工作 | 前置 |
|---|---|---|
| R0 | 新基线、差异核验、调用/所有权图、脚本能力清单与决定 | EC1 输入 |
| R1 | 发布计划／权限和 prepared 数据封装 | R0 |
| R2 | 场景配置草稿、纯准备与适用性继承 | R0 |
| R3 | 派生发布活动及结果/Workspace 面板迁移 | R1；面板配置使用 R2 |
| R4 | 预览、Workspace 迁移和模型 recipe 收敛 | R1–R3 |
| R5 | 运行期资产接口组合、作用域与结果寿命 | R0；不依赖 Editor R1–R4 |
| R6 | ScriptAbility、Lua 投影和语义/生成绑定 | R5 |
| R7 | PLAYER、Editor Run、独立 SDK 与生命周期实测 | R3–R6 |
| R8 | AGENTS、性能计数、依赖/安装/旧入口清理 | R1–R7 |
| R9 | 最终同一实现提交验证与封存 | R8 |

这是一个阶段的内部施工顺序，不要求每批新建库或测试程序。允许逻辑上独立的代码提交，但**不得并发执行构建与实机验证**。

## 6. 保持的工作区与验收边界

- EC1 报告中的实施目录为 `lux-engine-p12`，资格目录为 `lux-engine-ec1`。先核对实际 HEAD 和 dirty 状态，不按名字认定内容。
- 原 `lux-engine` 的 ProjectBuilder 用户修改仍受保护，未应用到新检出；未经用户授权不应用、提交或丢弃。
- 不修改 main、不合并、不删除分支、不发布 release；本轮结束停在 EC2 复审。
- P12 `PARTIAL_USER_WAIVER`、Linux/IME 未测、旧性能 PARTIAL 原样保留。不补旧慢算法 100 样本，不为本轮搭 Linux。
- 新增脚本和职责接口的真实验证不可用旧免验替代。缺少本轮必需运行证据，按该项 PARTIAL 报告，不捏造通过。
- 修改 modules 公共头时同步规定的三个安装 include 前缀，Android 同步不等于 Android 构建验证。[U01]

## 7. 成功标准

最后交付必须能同时回答：

1. 哪些原算法真正迁走，哪些旧入口实际删除？
2. 现在每份计划、占用、任务、结果、资产、Pane、代码引用分别由谁拥有？
3. 无 GUI 能否构造配置、发布产物；无 Editor 能否运行脚本并读到真实资产？
4. 新语言投影是否只转换调用/数据表示，底层 IO、codec、Task 与运行生命周期是否仍各只有一套？
5. 失效、取消、错误、关闭和晚到完成是否没有丢失已发生事实？
6. 引入的类型是否减少非法组合和依赖，而不是增加包装跳数？

逐项执行 [05_ACCEPTANCE.md](05_ACCEPTANCE.md)，依据 [07_SOURCE_INDEX.md](07_SOURCE_INDEX.md) 区分已读事实、旧审阅建议和本轮目标。

---

<!-- source: 01_BASELINE_AND_ARCHITECTURE.md -->

# 01　新基线、问题差异与架构裁定

## 1. EC1 结果继承：不能再按旧报告施工

| 上一次调查中的事项 | 新基线中的状态 | EC2 行动 |
|---|---|---|
| History 承担执行 | EC1 报告已变为 EditHistoryData + EditExecutor | 核验原回归与调用，保持，不再改回另一名字。 |
| PreparedSessionData | 报告已由 SessionPreparation 替代 | 保持一次消费与代码寿命；不新增转发执行者。 |
| EProjectAssetKind / 三类型路由 | 报告已引入 SourceAuthoring、开放工厂与 manifest v3 | 核验并继承。运行期脚本不得反过来依赖作者路由。 |
| ProjectCatalogSnapshot public 借用字段 | 报告已私有化并加同快照索引 | 使用现有快照，不再新增 CatalogReader 或第二索引。 |
| VCompiledSource | 已由 DerivedArtifact / IArtifactSource 取代，当前 EditorArtifacts 已使用新输入 | 开放输入已完成；**执行流程仍在 Application**，只迁剩余责任。[C04] |
| ResultIntent action+target | 当前 ResultsPane 已用 VResultIntent，动作绑定载荷 | 保留；面板仍持 Impl&，迁移此依赖。[C09] |
| 组件/Feature 适用性 | EC1 已有 AuthoringFacts/queryApplicability，当前配置 UI 已调用 | 复用，不再造第二要求解释器；从 UI 提取纯准备时继续使用。[C08] |
| Outliner/目录乘法扫描 | EC1 报告已整改 | 不为本轮再实现一套集合；只在相关变更时测回归。 |
| Scene 全快照重建 | EC1 明确保留 | 不把增量投影追加成本轮必选。 |

以上分别为当前读取源码事实或 EC1 提交报告事实，详见 [C03–C09]。报告不等于本次独立运行结果。

## 2. 新基线中仍应整改的责任

| ID | 当前事实/证据 | 真正需要改的东西 | 不应误改 |
|---|---|---|---|
| RD01 | ProjectPublication public 可变计划 + private owner 指针，头与旧版本相同。[C05] | 固定计划与 owner-thread 占用语义；计划不能准备后随意改写。 | ProjectWriteLease 的 RAII、原发布序列和已发布事实。 |
| RD02 | SceneConfigurationElement 公开 build/buildEdit，持 UI+builders+注册输入；CPP 仍含 presetFeatures、systemOptions。[C07,C08] | 草稿与纯准备，UI 不再成为唯一配置编译入口。 | 原 Builder 校验、未知配置、当前 AuthoringFacts。 |
| RD03 | DerivedArtifact 已开放，但 settleArtifacts 仍在 Application 排 Task、encodePak、发布与目录采用。[C04] | 发布业务和进度归原活动；Application 只接纳产品意图/观察。 | 已完成 IArtifactSource，不能重建第三套产物接口。 |
| RD04 | ResultsPane 已使用类型化意图，但依然直接读 writes/sessions/runs/Impl。[C09] | 正式数据观察及请求能力，面板独立构造。 | 合法 UI 回调延迟执行业务的原安全点。 |
| RD05 | MaterialPreviewStore 仍接收 MaterialCompileOperation；key 仍带 target；preview 返回 CompileResult。[C06,C10] | 配方、编译输入、目标采用、错误域分开，保留完整 live owner。 | GPU/Runtime retirement，不把预览拆成散落裸句柄。 |
| RD06 | WorkspaceStore 仍包含迁移、回退、layoutResult+catalog。[C11] | 纯转换/选择与真实读写分离，查询不能伪装成提交回执。 | 真实 IO 版本复查与 marker 幂等。 |
| RD07 | 旧调查指出模型 recipe codec 埋于 AssetImporter。[U02] | R0 复核当前完整实现后提取，明确 ModelImport 领域。 | 已有 ModelCooker 和 Process，不造万能导入器。 |
| RD08 | ProjectOpenData 带 write lease（旧调查）。[U02] | R0 复核，必要时改为拥有型准备名称并封装。 | 不要求可复制，更不能将活 Project 交 worker。 |

RD07/RD08 本次未重读完整实现，不能写成已取得新运行负例。实施者先核对它们的实际剩余工作。

## 3. 架构现在是否清楚

目录方向已清楚；职责仍有几处横跨多个实际变化理由。因此不重新设计五层，而要完成三种边界：

- **表示与处理**：draft/plan/immutable result 与 Builder/codec/operation 分开。
- **准备权限与业务数据**：permit、reservation、code lease 不是普通 DTO，不和可改字段混装。
- **宿主装配与可复用业务**：Application 可以组合，但不要自己实现另一个发布器或让通用面板读取全部私有状态。

新加入的游戏脚本让第四种边界必须明确：**运行能力与 Editor 产品能力不同**。

## 4. 三条消费链，不是同一万能 Context

```text
Editor 内置工具 / Editor 扩展
    -> EC1 的 SessionActivities / ProjectActivities / WorkbenchAccess
    -> Editor 作者与工作台提供者
    -> engine / modules 的原能力

C++ 游戏代码
    -> 运行期资产读取、组件、时间等公开能力
    -> Process / VFS / codec / Simulation

Lua / 已有原生脚本后端
    -> ScriptAbility 声明与语言投影
    -> 同一运行期能力 + 脚本调用的寿命/额度
    -> 原 Process / VFS / codec / Simulation
```

第二、三条路径都不得绕入 Editor。把 Lua 包一层 `ProjectActivities` 不叫复用基础设施，而是把游戏运行期绑到编辑器。

“同一接口”要求业务契约、结果含义和真正执行算法一致；不要求 C++ 的 `shared_ptr<const Asset>` 和 Lua 的稳定句柄具有同样的内存布局。语言投影不是应删除的兼容补丁，它有真实表示与寿命责任。

## 5. 层级与模块归属

| 责任 | 正式归属 | 依赖限制 |
|---|---|---|
| AssetId/AssetTypeId、资产布局、codec | modules/resource 原提供者 | 不含 Editor/ScriptRuntime/VM。 |
| AssetVfsView/AssetReadPort/loadAsset | modules/resource 与 engine/process 原提供者 | 不知道 Pane、History、Lua。 |
| ScriptAbility 契约/语义/生成，通用 Lua 值运输 | modules/function/script 原 core/lua | 不识别 Skeleton/Material/Editor 项目。 |
| ScriptSystem、awaitable、组件命令屏障 | engine/domain/simulation 原 scripting/builtin script | 不引入 Process 文件后端或 SceneRenderer 具体实现。 |
| 资产能力的运行期实例作用域与组合 | **目标：engine/scene/scripting/assets/** 的小型叶子接线模块 | 组合 process + script 协议；不反向让二者依赖该叶子。不得依赖渲染/GUI 才能读取。 |
| Lua 资产投影 | 同主题独立可选 target，或已有合法的相邻语言投影 target | native 合约不因 Lua 引入 lua.h；不是整个新脚本引擎。 |
| Editor 发布/草稿/预览/工作区 | 原 authoring/activities/workbench | 不成为游戏脚本 ABI。 |
| EditorApplication / PLAYER / 测试宿主 | 各自装配根 | 安装同一 runtime provider；不各写业务回调算法。 |

`engine/scene/scripting/assets` 是**拟议新叶子位置**，当前树中未取得这个目录已有实现的证据。R0 若发现已有同职责正式模块，应并入它并删除该新增计划；不得两处实现。放 scene 组合层是为了避免 domain/core 为访问文件反向依赖 process；不要求对象必须成为一个新的 SceneSystem，也不要求依赖 SceneRuntime 重资源。

源目录不是动态库数量。默认 native 能力接线一个 STATIC target、Lua 投影一个可选 target；若真实公开协议需独立 CPU 头 target，必须由实际无 Lua/无 Process 消费者证明。不得每种 handle 建一个库。

## 6. 游戏能力清单的分类方法

R0 从公开 provider、ScriptAbility 声明、生成列表、实际调用和注册位置建立清单，至少包括以下类别：

| 能力族 | 本轮定位 |
|---|---|
| 资产读取/解码/结果查看/释放 | 完整新实现与接通的主切片。 |
| 运行对象/组件读取与写命令 | 复用 DeferredScriptHost/原 typed component 绑定，至少一条真实组件用例。 |
| 时间/延时 | 复用 DelayAbility，作为另一能力独立组合的对照。 |
| 空间查询 | 盘点真实同步/异步、可用性和结果合同；本轮不强制新增完整 raycast 绑定。 |
| 音频、物理、输入、动画等 | 逐项标记已有绑定/缺绑定/不在范围；不由“可反射”推导“可安全脚本化”。 |
| 作者导入/编译/保存/布局/Pane | Editor-only，排除游戏脚本导出。 |

能力的可用性来自宿主明确提供和脚本声明匹配，不来自遍历全局 Application、DLL 符号或字符串查找服务。EC1 AuthoringFacts 留在作者编辑语义；ScriptApiCapability 继续表达运行期协议，两者不能混为一个 Is3DManager。

---

<!-- source: 02_RESPONSIBILITY_IMPLEMENTATION.md -->

# 02　职责迁移的具体设计与删除要求

## 1. RD01：ProjectPublicationPlan 与 PreparedProjectPublication

### 当前起点

文件：`editor/activities/project/include/lux/engine/editor/storage/ProjectPublication.hpp`、`src/ProjectStorage.cpp`、`src/ProjectPublicationOperation.cpp`。新 HEAD 的 public publication 定义仍混有固定计划与 private owner。[C05]

### 目标类型（新名称均为施工目标）

- `ProjectUpdate`：请求值，允许调用方在准入前编辑。
- `ProjectPublicationPlan`：准备成功后的固定 manifest、目标、版本、文件与包目录；私有数据/只读观察。不能改完 manifest 却继续使用旧 manifest_bytes。
- `PreparedProjectPublication`：move-only 的完整拥有单元，组合 plan 与唯一 owner 线程 reservation；能放弃但不能复制。可在同一分配中存放，不要求为两种语义新增两次堆分配。
- `ProjectPublicationOperation`：沿用现有执行者，拥有 prepared 对象、任务和真实结果；不新增同义 ProjectPublisher。
- `ProjectPublicationReceipt`：已发生的文件/清单事实以及采用输入；与 plan identity/precondition 对应，不因 UI 状态推导。

### 计划、占用与销毁

1. 先验证输入、预算、路径、immutable reuse 和完整候选，再取得/发布占用；不能失败后留下 publishing_。
2. Plan 给 worker 的只读拥有视图不具有解除 reservation 的权限。worker 不保存 `ProjectStorage*`。
3. Prepared 的最终释放在原 owner 有效且调用保护内进行。不同 executor 不得建立独立 busy 来控制同一项目。
4. move construction 只转移一次权限；move assignment 必须先将旧完整拥有单元移至局部，按规定顺序清理。禁止字段逐个覆盖导致旧 code/source 提前失效。
5. 放弃尚未开始的计划可以释放占用；在途发布由原 Operation 等到真实结果/Unknown reconciliation，不得把析构当作磁盘回滚。
6. 提交 manifest 后的采用失败是可诊断/可重试的已发生事实，不允许重新生成新的计划冒充第一次成功。
7. 同一 immutable 资产包可被重用与可变作者保存不同；不合并两者的版本策略。

### 精确处置

删除现有 `ProjectPublication` 的 public 可写计划字段接口与旧名称的兼容入口。迁移 ProjectStorage、原 Operation、导入、派生产物、项目创建/设置、所有 SDK 和测试调用。

先查 `publishProjectFiles()` 的生产消费者：若是保留的崩溃恢复协议则复用；若与 Operation 重复实现同一提交序列，合并实际文件步骤。不能按名字删除恢复 helper，也不能同时维护第二份业务顺序。

ProjectWriteLease 与 Prepared 的发布占用不是同一权限，前者是 OS 项目写锁，后者是本 owner 的有序业务准入。两者保留准确分工，不合并成一个 bool。

## 2. RD02：SceneConfigurationDraft 与准备函数

### 目标

```text
已有配置/新建参数
    -> 拥有型 SceneConfigurationDraft
    -> prepareSceneConfiguration(draft, 固定注册/能力描述)
    -> 原 Scene/Simulation Builders
    -> 正式配置/SceneSetConfiguration

UI 只维护草稿、结束控件编辑、展示诊断、发出领域提交
```

### 草稿必须包含

场景显示名称（仅实际可改时）、声明 schema、稳定 SystemInstanceId、系统类型/配置 schema 与版本/拥有字节、provider 绑定、construction/execution/scene/channel 关系、viewport 选择，以及修改现有配置时的原内容戳/原描述保留。

不要用 `map<string,any>` 代替这些语义；不要每种字段创建独立模块。优先复用引擎已有的描述值和边类型。未知系统/Feature 配置字节与原版本完整保留，不能因 UI 没控件就写默认值覆盖。

### 精确迁移

- 从 `SceneConfigurationElement.cpp` 迁出 `SystemElement` 中正式描述构造、默认 Feature 配方选择、provider 解析、关系验证；私有控件类保留布局和局部输入。
- `build()` 不再读取全部控件并成为唯一编译器。改为控件结束编辑后捕获 draft，再调用同一准备函数；不要留下两个 build 实现。
- `load()` 分为正规配置到 draft、draft 安装到离树控件两步。失败保持原挂载表单/source/code owner。
- `SceneCreationConfiguration` 从 UI 头迁到其真实纯值/准备提供者；拆掉 `CONTROL_FAILURE` 与模型准备错误混装，UI 可以汇总两者。
- `SceneProviderOption` 的 string_view 仅用于同调用借用；跨帧 draft/准备结果要拥有名称或保有固定注册快照。不能从临时容器借用。
- 复用 EC1 的 `AuthoringFacts/queryApplicability` 与原 Builder，不复制依赖解析和 Is2D/Is3D 表。

### 实际分区与预设

当前 UI 新建明确只提供 single partition，且 `systemOptions` 仍以 single 查询。[C08] 新纯函数应显式接受目标分区契约：

- 新建默认 single 是产品预设，不是所有来源的隐式事实。
- 对既有 indexed World 不支持的结构编辑继续准确拒绝，不把列出候选当成新增索引编辑功能。
- 2D/3D 预设只是生成一份初始 draft；不影响正交/透视、活动查询后端、分区存储的独立含义。
- 已存在的不适用/未知配置应显示并保留诊断，不能静默丢弃。

草稿/只依赖纯描述的函数归 `authoring/scene`；需要运行注册列表/Feature default codec 的准备归 `activities/scene`，但仍不得要求实际 Renderer 或 Root。工作台转输入。**这些 Editor 草稿不自动成为游戏脚本 API**；游戏直接使用引擎已有运行描述或能力。

## 3. RD03：把派生产物发布从 Application 迁回实际活动

当前已使用 `DerivedArtifact`，不要恢复 VCompiledSource 或再创建并行 AnyCompiledObject。[C04]

### 活动入口

由现有项目/持久化提供者接收：固定 DerivedArtifact、准确的源绑定/采用政策、目标项目与预算。返回稳定操作 ID 或原拥有型操作。输入拒绝不丢调用方唯一载荷；接受后 UI 可以消失，操作仍有 owner。

### 原流程应迁走的部分

`EditorArtifacts.cpp::settleArtifacts()` 中的：

- 源 key/binding 与目标验证；
- package 编码与源摘要；
- 文件目标 reserve/provide/settle/reconcile；
- 已发布包目录读取；
- manifest 准备/写入/采用/确认；
- 编码和读取阶段的回调结果记录。

保留 Application 的产品动作选择、结果观察及退出协调，不在 Impl 中维护另一个上述状态机。

### 复用策略必须先实现再迁消费者

1. 审核 `ProjectPublicationOperation` 已有 FILES/PACKAGES/MANIFEST/ADOPT 顺序。
2. 提供固定包输入及“已发布但尚未登记”结果时，扩展原执行者的准确输入/阶段，不重新发布同一文件。
3. 如果现有唯一多文件协议和 derived 提交的不同源保存政策不能直接共用，在原模块提取共同发布步骤，两个有明确领域语义的入口组合它；不可复制核心算法。
4. 源在编译后继续保存时，派生产物目录采用只能更新其 cooked 事实，不倒退 source_digest/checkpoint。
5. 继续支持“文件成功、目录失败”、Unknown、取消后晚到结果和明确确认；结果查询不会因为窗口关闭而消失。

`ArtifactPresentation` 最终只保留显示所需的操作 ID/拥有型只读摘要，不再含执行所需的两个 ticket、多组 encoding/reading 状态和私有 Task。

## 4. RD04：WorkspacePane / ResultsPane 不再使用 AppImpl

### 最终约束

通用面板能在独立安装消费者中构造，不包含 `EditorApplicationImpl.hpp`；原内置面板使用同一个公开入口。

- WorkspaceView 消费布局目录/迁移结果的只读观察，发出类型化 Workspace 请求。
- ResultsView 消费来源明确的 Save/Run/Import/Publication 观察，并发送现有类型化动作或命令。
- 跨层的打开显示/恢复/退出继续由 Application 组合，不能为“面板都在 workbench”而将 ViewHost 拉进底层内容服务。

不新增 `IApplicationAccess`、巨大 getters 接口、SharedApplicationState。已有具体 provider 足够时直接注入引用。确需跨域呈现汇总时只提供 immutable summary 和有限动作集合；它不拥有第二份业务结果、不自行确认底层记录。

### 更新和线程

- UI 回调只入意图，原 owner 在安全点执行；队列满必须明确，不重发部分已交付动作。
- draw 不读取文件、不启动编码、不调用 `waitUntil`。
- 若已有 revision，可在 update 中生成一次共享展示快照；没有收益证据不每字段加缓存。
- snapshot 失败保留上次显示并标记暂时不可用，不报告“空列表”。
- 终态结果与行 ID 有完整域/代际关联，不能只取 slot 或把 uint64 截成 int 用作业务身份。

原 `VResultIntent/VWorkspaceIntent` 保持动作绑定载荷，迁到恰当实际契约位置，旧 private 定义同批删除。

## 5. RD05：MaterialPreview 的配方、产物和采用

### 目标分工

- **MaterialCompileInputKey**（目标名）：content/settings/environment，描述编译什么。
- **CompiledMaterial**：经正确构造的固定输入、artifact 与 bytes；只读关联，不带某个窗口的唯一 target。
- **PreviewAdoptionKey**（目标名）：完整预览目标身份/代次、期望输入、配方/环境代次，属于单预览采用。
- **MaterialPreview**：现有单目标活预览 owner 的最终语义名，继续拥有 runtime lease、候选、已接受状态、相机和退休。
- **预览配方/构造函数**：现有纯 ScenePackage 或紧凑 recipe；默认球体是一个内置配方，不在资源采用循环中决定。

### 必须迁移的算法和接口

- 将 sphereImage/previewWorld/默认 Feature、灯光和相机构造移到同主题配方源文件，不放入另一个全局 PreviewManager。
- `receive(const MaterialCompileOperation&, ...)` 改为接收拥有型完成结果与目标采用关联。编译 operation 仍独占控制权，预览不取消或窥探 task。
- 错误拆成编译错误与预览/资源错误；navigate/reset 不再伪装为编译请求失败。
- 原 target 参数从纯编译结果身份退出；每个目标分别保持期望代次，旧完成无法覆盖新目标。不得简单删除 target 判断。
- 两个预览可消费同一固定编译产物；目标分别独立，关闭其中一个不能销毁另一个所需的资源。
- `CompiledMaterial/CompiledFlow` 关联由 compiler/合法工厂构造，公共只读访问。不要允许外部随意更换 bytes 却保留另一份 artifact/key。

不要求新的全局编译缓存。共享同一结果的能力与自动去重是两件事。编译 settings 中真正影响输出或成功条件的部分保持 key 语义，不为删 target 顺带改变环境/额度的验证。

## 6. RD06：Workspace 数据转换、政策与 IO

将原 `LegacyWorkspaceImporter.cpp` 内的纯 TOML/INI→布局/恢复转换提取为可对固定字节调用的函数。输入应拥有每个文件的相对路径、字节和已验证版本；输出为固定迁移计划。

存储负责采集真实输入及验证 current version；迁移推进复用原 coordinator 与 marker。每次重试都必须证明来源仍相同，但版本未变时不必重复全部解析。目录枚举不完整、权限失败不得变为空目录。

- 只选明确 selected 的旧布局产生恢复，所有合法布局保留。
- 已同源写入的新布局、用户后来修改的 label/recovery、完成 marker 不能被升级静默覆盖。
- `layoutResult()` 与 `LayoutCommitReceipt` 的 pending + catalog 混合接口退出：使用原 WriteCoordinator status，显式 `listLayouts/refresh` 做 IO。
- 不新增一个只有一行 status 转发的 WorkspaceStatusManager。
- 纯 fallback 决策可以单独函数；便利的读+选择入口名称明确、成本明确。

## 7. RD07/RD08：模型 recipe 与 Project 打开准备

R0 核对当前剩余算法后：

- 将 ModelImportRecipe 的值、decode/encode 从 AssetImporter 的 Load/Encode 内提取，放原模型导入主题；复用 ModelCooker。
- 原 AssetImporter 若确实只有模型能力，最终以 ModelImportOperation/ModelImporter 的单一准确名称提供（R0 根据其是一请求 owner 还是多记录 owner 选择一个），不同时保留两个同义公开入口。
- 字节读取、CPU cook、项目发表仍用原 Process 和发布者；不建立 ImporterRegistry，除非本轮真实第二种导入消费者无法用现有流程表达。
- ProjectOpenData 若仍含 write lease，改为拥有型 `PreparedProjectOpen` 并封装消费规则。准备值不等于可复制 POD；worker 可生成尚未采用的拥有结果，但不能销毁活 Project。
- 名称变化同步安装 SDK、生成器、测试和应用，不留下 alias。

## 8. 本轮明确不拆

EditHistoryData/EditExecutor（EC1 已做）、SessionState/EditGate、SessionStore、三领域 Session、真实编译操作、WriteCoordinator、SceneProjection/Hub、SceneRuntime、DetachedView/CodeLease 的必要资源拥有单元全部保留。

如果发现它们中的具体违规，按局部证据处理，不借“重新分配职责”把每个 getter 变成 Processor 方法。每个新增生产类型必须有一个原责任或必要新边界与之对应；没有对应者不新增。

---

<!-- source: 03_GAME_SCRIPT_CAPABILITIES.md -->

# 03　游戏运行期能力与脚本投影：完整实施契约

## 1. 不是 Editor Scripting

本轮要支持的是游戏脚本读取运行资产、取得受控结果，并在合法运行区域中使用它。以下路径必须不含 Editor：

```text
脚本源/编译产物
    -> 现有 LuaScriptBackend / ScriptSystem
    -> 已准备 ScriptAbility 方法
    -> 运行期 provider
    -> AssetReadPort / loadAsset / 原 codec / Process
```

禁止通过 Editor 的 ProjectStorage 取得文件后端；禁止脚本修改项目 manifest、调用 Save As、创建 Pane、使用 History。Editor Play 可以向运行期提供它已装配的资产输入，但运行中的脚本看不到这个输入来自 Editor 还是 PLAYER。

源资产与 cooked 资产要明确：游戏默认读取宿主挂载的运行资产。不会默认得到 `.lux/editor`、作者源码、项目根目录或任意绝对路径。宿主确实需要开放调试数据时，用显式独立权限，不改变默认契约。

## 2. 当前已经有的基础，不重做

| 已读代码 | 能力 | 本轮要求 |
|---|---|---|
| AssetLoadSender.hpp | `loadAsset<ConcreteAsset>` 组合 AssetReadPort、CpuScheduler、TAssetSerDeser，返回拥有型结果 | IO/解码只有这条原算法或已有 codec registry 的对应正式算法，不复制在 Lua callback 中。[C18] |
| VfsAssetReadEndpoint.hpp | 有界读取，请求保活 endpoint；TaskScope 须活到接受工作结束 | 直接借用/组合，不自建磁盘线程或脚本文件后端。[C19] |
| ScriptAbility.hpp | QUERY/异步方法描述、语义参数、erased completion、值寿命标签 | 新方法沿这套契约，函数表在真实 ABI 边界不是违规。[C12] |
| ScriptAbilityInvocation.hpp | 外部异步结果先预留 awaitable；仅 void 或已声明且 trivially-copyable 类型 | 不能运输 shared_ptr/AssetBlob/任意 C++ 对象。[C13] |
| ScriptLocalAsync.hpp | 只有 ScriptTimers 铸造本地异步快捷入口 | 资产 IO 使用外部异步通道，不伪造 timer 授权。[C14] |
| ScriptSystem.hpp | 明确 execution region、stable point、lifecycle、外部完成与恢复预算 | 不从 Process 回调直接恢复 VM，不另建 pump。[C20] |
| DeferredScriptHost.hpp | 组件读取和受控 EcsCommandWriter，既有命令屏障 | 资产完成后写组件仍走原区域与命令，不传 Registry 给 worker。[C21] |
| ScriptAbilityLua.hpp | `makeLuaValueOperation`、bounded codec、Lua method projection | 新值通过同一生成/投影；不在 LuaBackend 中加资产类别 switch。[C17] |
| LuaScriptBackend.hpp | 预备方法/事件容量、value/ability 声明、统计和后端描述 | 正确声明类型与额度，不能降低准入来“支持任何类型”。[C16] |
| DelayAbility.hpp + lux_script_abilities | 真正已存在的能力声明/生成例子 | 新能力按同一构建接入，Delay 保持原实现。[C22,C23] |

本次没有穷尽读取所有游戏能力声明。R0 先检索是否已有资产能力 provider；有等价实现即完善并复用，不按本文件的目标名再造一个。

## 3. 原生接口与脚本接口如何共享

### 3.1 原生 C++ 层

继续支持现有直接调用（已有 API）：

```cpp
auto work = process::asset_loading::loadAsset<asset::SkeletonAsset>(
    reads,
    execution.cpu(),
    id,
    limits,
    stop
);
```

这是 sender，不是已完成资产，也不能直接在当前 Hook 中同步等待。C++ 宿主通过既有 Task/TaskScope 消费结果。

脚本 provider 调用同一读取与解码实现。它额外负责的是**语言不可直接表达的调用寿命、额度、稳定句柄和结果运输**，不是第二份资产加载、缓存、目录或序列化实现。

### 3.2 本轮脚本必须提供的能力

下表是**目标签名与语义**，具体用户语法须沿现有生成器，不是已经存在的 API。最终交付包含真正可运行的 `.lua`，不能照抄此伪接口而不接生成链。

| 操作 | 语义类别 | 约束 |
|---|---|---|
| `readAsset(id)` | ASYNC | 按原 AssetReadPort 读完整资产镜像，返回可运输 outcome；不等于 GPU resident。 |
| `describeAsset(handle)` | QUERY | 只读已取得镜像/类型/尺寸/来源标识，不发起 IO；未知元数据明确缺失。 |
| `copyAssetBytes(handle, offset, count)` 或等价有界访问 | QUERY | 仅复制已持有缓冲的指定范围到受控 Lua 值，检查溢出和最大返回字节；不是每帧全量 tostring。 |
| `releaseAsset(handle)` | COMMAND/同步资源操作 | 释放该脚本作用域的原生结果持有；不卸载其他使用者的资产，也不等待 GPU。 |
| 一种正式 typed 读取，如 Skeleton | ASYNC + QUERY | 用原 SkeletonAsset codec 返回 typed 句柄；可读骨骼数/有限字段，不复制骨骼数据类型或 codec。 |
| 既有 DelayAbility + 一种正式组件读/写 | 复用验证 | 与资产读取组合，证明有 query、受控 command、awaitable 三种调用，而非资产专用 VM 特例。 |

“元数据查询”只对 provider 已有固定目录/已加载结果成立。若底层没有廉价目录 API，不新增一个假 `exists()`，也不在同步方法中偷偷读磁盘。按 AssetId 读取失败即可准确报告；虚拟路径解析若提供，必须复用原 VFS、在已知快照上进行，并区别缺失与不可访问。

通用 readAsset 提供镜像；typed accessor 可由 Skeleton 的独立能力贡献提供。通用 VM 与资产读取核不得 hardcode Skeleton/Material/Model。没有已存在动态 codec registry 时，先用静态 `loadAsset<T>` 的领域投影；不为本轮构造巨型万能解码器。

### 3.3 操作与结果的命名

- read 成功只代表取得字节；decode 成功代表取得合法 CPU 资产；renderer 接纳/驻留/GPU 完成是其他事实。
- request/start 表示接纳，complete 表示该请求终态，release 表示释放本作用域引用。
- 数据对象可保留 size/find/query 等自身观察方法；它不在 getter 内读取文件或调度任务。
- 原生错误按原类型保留；脚本边界映射为稳定的 domain/code/必要固定信息，不使用日志文案判断。

## 4. 结果为什么需要句柄，而不是 SharedBytes 直接返回

当前外部异步 resume 通道的静态要求不能安全传递带引用计数和析构的 C++ 值。[C13]

目标：

```text
原生 AssetBlob / shared_ptr<const T>
    -> 原生脚本结果 owner 保活
    -> 小型 ScriptAssetHandle（域 + slot + generation）
    -> 现有可运输结果槽
    -> Lua 中有明确语义的只读句柄值
```

### 4.1 句柄的完整契约

- 全部身份来自原生 owner。脚本不得提供 context 指针、scope 指针或伪造权限声明。
- 不把指针转整数，不把 AssetId/uint64 一律转 Lua double；128-bit AssetId 保持原精度。
- handle 至少区分所属 provider/作用域与槽代次；同槽复用后旧 handle 拒绝。
- scope/完整实例身份使用既有身份机制；若没有合适分配器，新增仅负责这一真实域的冷路径分配，不用每 DLL header static 伪造全局唯一。
- Lua 表示优先沿已有 `LuaValueOperation/TLuaValueCodec` 的 typed 值机制。纯 table 无法被当作可信 native handle；decode 每次检查 type/size/域/代次，不只检查字段存在。
- typed 句柄还验证对应 AssetTypeId/语义表示版本，类型不匹配不能退化为 raw reinterpret_cast。

### 4.2 结果表不是另一个资产缓存

它只保活当前调用获得的原生结果并将句柄关联到它；不重做依赖加载、路径解析、全局去重或后台线程。

同一 immutable bytes/asset 可被多个原生 owner 共享。Lua 的 token 本身不复制大 buffer。逻辑保留字节计入本作用域预算，不能仅因底层共享就让脚本绕过额度。

### 4.3 明确采用 scope-owned token

本轮默认语义：结果由原生脚本实例作用域拥有，handle 是对此记录的稳定引用，不是拷贝一次就创建一份独立释放责任的 shared_ptr。

- 同一 handle 的 Lua 赋值/拷贝是别名。
- 显式 release 使该记录的所有旧别名失效；重复 release 返回明确已失效/已释放结果，不发生第二次释放。
- Lua `__gc` 不直接取消其他别名的所有权，不调用阻塞 IO/业务/Registry；不把 GC 时机当作容量和关闭协议。
- 实例关闭必须撤销其结果域，并释放所有已交付但未显式释放的记录。
- 跨实例共享必须通过正式重新获取或明确 transfer/retain 协议；本轮不暗中支持 transferable handle。

这使容量和脚本销毁行为可确定。需要一般共享脚本对象引用计数时另作明确需求，不能偷偷改变此合同。

### 4.4 字节额度的层次必须诚实

已读 `ReadAssetImage` 只有 AssetId，VfsAssetReadEndpoint 配置只有 request_capacity，执行段调用 `vfs.open(id)`。[C18,C19,C27] 这**不足以证明**单请求读取前就按脚本 max_bytes 限制了下游临时分配。

R5 必须沿真实 provider 核对：镜像大小在何时确定、是否先于分配/解压验证、固定挂载版本怎样保持。脚本结果保留预算、codec decode 限额、后端瞬时内存和整个进程 RSS 分别报告。

有现成 provider 限额即复用；暴露的读取需要新的字节上界而旧协议不能表达时，应在原 ReadAssetImage/endpoint/provider 的正式操作中补准确限额并迁移消费者，不新增 Lua 专用文件读取。不允许仅在巨量字节已分配后做 size 判断，就宣称“全链路严格内存上限”。无须借此建设进程级 OOM 恢复系统。

### 4.5 目标类型示意（非现有可编译 API）

```cpp
// 是运输身份，不拥有 Asset，不包含 C++ 指针。
struct ScriptAssetHandle final
{
    std::uint64_t domain{};
    std::uint32_t slot{};
    std::uint32_t generation{};
};

// 实际状态和语义 ID 用正式声明生成；下面只表达关联。
struct ScriptAssetReadOutcome final
{
    EScriptAssetReadStatus status{};
    ScriptAssetHandle handle{};
    std::uint32_t error_domain{};
    std::uint32_t error_code{};
};
```

这不是新数据袋任意组合的许可。结果由 native factory 构造，成功/失败关系校验；进入 Lua 为只读观察。实际 ABI 必须验证语义 ID、字段布局/对齐、保留字段初始化和版本，不以 `sizeof` 相同认定类型相同。

范围返回值使用已存在的 bounded 语义值或有限 `AssetByteChunk` 投影；不无限字符串化整个资产。除 transport 必须的小值，域内继续使用原 AssetId/AssetTypeId，不再复制一套游戏 UUID。

## 5. 实例作用域：不能从脚本传入一个 caller_id 解决

这是 R0/R5 必须作出的具体核验，不能假定已有能力全部满足。

当前公开 `ScriptApiCapabilityPublication` 提供 provider context/dispatch，`ScriptSystem::create` 接收能力与后端数组；已读接口不足以证明任意新资产 provider 都能取得可靠的逐实例释放通知。[C15,C20]

实施顺序：

1. 读取当前 ScriptPreparer/ScriptInstances/ScriptBackend 的完整相关调用；定位实际 ScriptInstanceId、prepared method、能力 lease 以及实例撤销位置。
2. 若已有逐实例 provider 准备/释放能力，直接用它产生 `ScriptAssetScope`（目标名）。
3. 若没有，在**原能力准备/实例生命周期**处补一个窄的 scope prepare/revoke 关系：由真正的 native 实例身份铸造 context，保存到原 prepared binding，实例退役先标记 scope stopping，最后等接受工作回收。不是全局 ScriptScopeRegistry，也不是第二个 ScriptSystem。
4. 固定能力描述和生成 traits 不变成每帧数据。只在冷准备时绑定当前 scope，热调用直接用已准备 typed/erased method。
5. 不使用当前焦点、全局 CurrentScript、TLS 裸状态、Lua 可写字段或整数传参判断真实调用者。
6. 同域多个脚本只能访问各自句柄；旧 ScriptInstanceId 的结果不投给新代际实例。

这项小扩展可能改变 script 的内部准备合同或相关 ABI，必须记录精确影响并更新合法消费者；**不得为避免改动原模块而使用不可靠的用户态 caller 参数**。Editor V8 与 Script ABI 是两套边界，不因名称相似一并升级；真实公共变化才升级对应指纹/版本。

## 6. 异步接纳、结果和清理的完整次序

```text
脚本调用已准备的方法
 -> 检查当前能力与输入
 -> 原 awaitable 预算接纳（已有机制）
 -> provider 为本请求预留结果/字节/记录额度
 -> 复制或持有稳定输入，启动原 TaskScope/loadAsset
 -> IO/CPU 完成，回调不进入 Lua、不写 ECS
 -> 原完成入口保存运输结果或进行纯保管步骤
 -> ScriptSystem 的既有安全点采用并恢复
 -> 脚本读取 outcome/handle
 -> 显式 release 或实例结束清理
```

### 6.1 两种失败不能混淆

**调用/协议无法接纳**：能力缺失、schema 不符、方法输入错误、awaitable/请求预算不足、实例 stopping。通过现有 start/call error 返回；不得先启动工作再说拒绝。

**已接纳的业务失败**：资产不存在、格式不匹配、读取 IO 错误、取消、预算被具体输入超出。这些应当能以脚本可检查的 `AssetReadOutcome`（目标固定值）表达；普通“文件没找到”不应只能触发 ScriptSystem instance fault。

目标 outcome 是 `status + optional-valid handle + 固定错误域/码` 的有约束结果表示。native 工厂构造、Lua 只读观察；成功必有合法 handle，失败不含有效 handle。它必须满足当前语义/字节运输要求。不要将 std::variant<std::string,...> 直接 memcpy 作为所谓 trivially copyable result。

协议自身损坏、底层异步通道失效与业务 NOT_FOUND 分开记录。保留原完整 native 错误用于宿主诊断；脚本传输不携带 std::any、外部异常对象或临时字符串视图。

### 6.2 预留与交付失败

- 在请求获准前预留完成保管能力；接受后不能因为 UI BUSY 或临时执行期状态而丢完成。
- 允许底层同步完成，但仅入原完成路径，不能在 start 返回前任意重入脚本。
- 结果准备后若 completion 已 stale，立即在合法 native 清理位置释放未交付句柄/额度，不留下孤儿 slot。
- completion 已接受但实例在恢复前销毁时，scope 撤销必须回收结果。只在发送前检查 `active()` 不足以证明这一时序。
- 生命周期取消停止用户交付，但原 IO/task 的所有权和最后清理仍需要完整结束。
- cancel/release 不调用其他作用域 TaskScope.stop；不得停止共用 provider 的全部请求来取消单脚本。

### 6.3 owner 线程与销毁顺序

给出最终实际顺序图，至少包含：原资产 endpoint、TaskScope、script scope、awaitable、Lua coroutine、typed result、code owner。

- VM 和 ScriptSystem 只在其 owner/lifecycle 位置触达。
- 需要异步的 native scope 记录由已存在 TaskScope/完成引用保活；停止后业务能力撤销，但不能提前销毁 worker 要回写的记录。
- 服务/endpoint 整体 close 由宿主在实例退休和完成结清后进行；借用 endpoint 的作用域不擅自关闭公共 endpoint。
- 插件提供的 codec/value/deleter/code pin 必须覆盖 result 的最后析构，含移动赋值和失效结果清理。游戏侧使用原 engine/plugin 的代码保活，不得为方便引用 `editor::contracts::CodeLease`。
- 原 Scene/Simulation 的命令屏障仍唯一。数据已加载不授权 worker 创建实体。

## 7. 三种能力形式分别实现

### QUERY

只读取当前合法快照或已持有结果，不 IO、不等待、不分配大缓冲。必要的有界值拷贝明确声明。读取组件返回值/只在本 step 有效的借用；跨 await 不能保存组件地址。

### COMMAND

修改组件通过已有 DeferredScriptHost/EcsCommandWriter 的合法执行区域。回调不直接在 EnTT observer 内破坏 pool。命令接纳与屏障实际执行分别报告，不能把 true 当成世界已经更新。

### ASYNC

使用 ScriptAbility 外部完成、原 Process 和现有 resume 机制。不得把任意 disk IO 放到 local timer 特权路径，不自建 Lua promise 系统/线程队列。

原生 static/C++ 脚本与 Lua 投影使用同一方法语义和错误映射。不要给 Lua 每个操作另写一份验证、文件解析和状态机。

## 8. Lua C 边界

仓库已有 `LuaBoundary.h`：typed worker 正常返回、C++ 临时值销毁后，由 C boundary 处理 RETURN/SUSPEND/ERROR。[C26]

所有新增绑定继续走这个边界与既有生成器。不能在持有 RAII scope/unique_ptr/CodeLease 的 C++ 栈上直接 `lua_error`/yield，不能靠普通 C++ catch 捕获 Lua 的非局部跳转。Lua 5.5 标准手册说明错误/yield 与 C continuation 的约束；本仓库的实际补丁和边界实现是落地依据。[W01]

- 注册/marshal 不手写另一套 metatable 管理器，有现成 bounded TLuaValueCodec 就复用。
- type operation 的 size/alignment/representation/version 必须匹配；错误签名在准备时拒绝。
- 业务 expected 错误转换一次，不按日志字符串二次推断。
- 不把 raw native pointer 放入 lightuserdata 当可自由持久化的能力。
- 环境默认不因新资产功能开放任意 `io/os/debug/package.loadlib`；沿原宿主配置明确白名单。没有审计 VM 全部攻击面，不宣称安全沙箱已认证。

## 9. 最小可工作的交付样例

同一组项目运行包和用户逻辑分别在独立 PLAYER 与 Editor Run 执行：

1. 请求一个真实存在的 SkeletonAsset 或另一个现有 typed CPU 资产；原 codec 实际读取。
2. 读取有限元信息，检查完整 ID/类型/字段与 C++ 对照一致。
3. 等待一次 Delay（原机制），再次读取合法句柄。
4. 在合法 step 中读取/修改一个已显式开放的实际组件，通过原命令屏障观察结果。
5. 释放资产，再访问旧句柄得到准确失效；退出无悬挂任务/awaitable/结果记录。
6. 同一脚本请求缺失资产时，能够查看业务失败并继续执行；不以单个 false/空日志代替。

另有独立无 SceneRender/GPU 的 C++ 消费者使用同一 asset read/codec 能力。不得要求为了读取 Skeleton 注册材质编译器或安装整个 Editor。

样例和能力是实际产品模块的消费者，不是测试程序中的另一套加载器。最终提供真实 `.lua`、生成声明、宿主接线、包输入和期望输出。

## 10. 不在本轮冒称完成

没有 Lua 作者文档/IDE、完整 script attachment 产品体验、断点调试、任意动态插件热卸载、全场景/物理/音频 API、任意 C++ 类型零成本反射绑定、可靠沙箱认证、Android/Linux 实测等新声明。

可扩展性证明是“一个资产能力和既有另一能力通过统一契约独立组合，新增普通能力不修改通用 VM”，不是“从此任何函数都能无成本脚本化”。

---

<!-- source: 04_WORK_PACKAGES_AND_FILES.md -->

# 04　逐批施工、类型处置与实际文件范围

## 1. R0：必须先形成可实施的真实清单

读取当前 AGENTS、docs/editor-quality.md、EC1 receipt/report、责任调查和本包。只检查新基线，不还原旧 HEAD。

必须输出到唯一 ec2 账本：

- 当前 tracked SHA、实现父提交、worktree 与用户 diff 状态。
- RD01–RD08 的现有符号/方法/成员/文件/直接和传递消费者；已在 EC1 消除者标 REMOVED，不重复施工。
- 脚本链：来源挂载 → ScriptSystem 构造 → capability 准备 → method bind → invoke/start → completion → stable resume → 实例退役；标每步真实 owner。
- 资产链：原 AssetReadPort/loadAsset、VFS 挂载来源、typed codec、read budget 和 native result 寿命；有没有现存等价 ScriptAbility。
- per-instance capability prepare/release 是否存在；handle 的数据运输方式是否已覆盖固定结构；对不足只给一个窄补正方案。
- Native/Editor-only/Script-exportable/已绑定/本轮绑定/未绑定功能表，按语义不是按函数名 grep 决定。
- 真实 target 图及 include/link 闭包，明确拟新增 assets 叶子不会造成 domain→scene/process 反向边。

R0 不以“环境中全部测试通过”代替上表，也不拖成一次全仓穷尽 AST 工程。出现已定位的阻塞先修其前置；不得拿旧未测项作为理由扩大本轮。

## 2. 逐文件施工范围

下表中的新名称/新文件均为目标。能在原语义文件内表达的，不为每个小值新建文件。迁移旧 public 名称时同步全部消费者，不保留 shim。

| 当前文件/类型 | 施工动作 | 最终责任 |
|---|---|---|
| activities/project/.../ProjectPublication.hpp | ProjectPublication 分为固定 plan + PreparedProjectPublication；receipt 合同与之对应 | 计划/准备权限 |
| activities/project/src/ProjectStorage.cpp | 只用新 prepared 访问器，保留 publishing_ 唯一权威、mount/目录采用 | 项目 owner |
| activities/project/src/ProjectPublicationOperation.cpp | 复用原步骤，消除 Application 的重复执行；必要输入支持只加这里 | 发布执行 |
| activities/project/.../ProjectOpenData.hpp | 实际持 lease 时改 PreparedProjectOpen，旧名删除 | 拥有型打开准备 |
| activities/project/src/AssetImporter.cpp | 模型 recipe 纯值/codec 提取；领域命名与消费者更新 | 模型导入活动 |
| authoring/scene 新/原配置值头 | draft/稳定关系/来源值，准确头依赖 | 纯作者配置 |
| activities/scene 新/原配置准备源 | 无 Root 的默认值、固定描述准备；复用原 builders/facts | 配置准备算法 |
| workbench/scene/.../SceneConfigurationElement.hpp/.cpp | 移出正式描述构建/预设/领域错误；保留 UI scratch/捕获/控件寿命 | 配置界面 |
| workbench/scene/SceneCreationView.cpp / SceneConfigurationView.cpp | 使用同一 draft/prepare/commit 流程 | 用户动作 |
| application/src/EditorArtifacts.cpp | 删除打包/Task/文件/manifest 核心顺序，保留发起/观察薄组合 | 产品接线 |
| application/.../EditorApplicationImpl.hpp | 执行字段归活动，删除冗余 flags/回调记录；保持其他用例原 owner | 装配/有限 UI 状态 |
| application/src/EditorResults.cpp / EditorWorkspace.cpp | 真实面板构造迁 workbench 主题；不再访问 Impl | 通用视图 |
| workbench 对应 result/workspace 主题（目标，可合并相邻） | 单一 View+必要视图值/请求；不建全局 ResultsManager | 展示和动作 |
| activities/material/.../MaterialCompilation.hpp / CPP | input key 与 target 分域，封装 CompiledMaterial；迁错误 | 编译结果 |
| activities/flow/.../FlowCompilationService.hpp / CPP | 按实际关联封装 CompiledFlow，不修改固定 object 重链策略 | 编译结果 |
| activities/material/.../MaterialPreviewStore.hpp/.cpp | 最终语义为 MaterialPreview；接收完成值；配方移同主题源 | 活预览 |
| activities/workspace/.../WorkspaceStore.hpp/.cpp | status 与目录 IO 分离，去掉误名 layout receipt | 文件存储 |
| activities/workspace/src/LegacyWorkspaceImporter.cpp | 纯转换与读取/推进分开，保留原指纹、来源、marker | 迁移 |
| engine/process/asset_loading/ 原文件 | 优先不改；确有可复用低层缺口才补，不 include script/Editor | 原生资产读取 |
| engine/scene/scripting/assets/（目标） | ScriptAssetAccess/scope/请求结果与能力声明，两个可分离 native/Lua target | 游戏能力组合 |
| modules/function/script/core/lua 的原能力与生成设施 | 原样复用；固定值 marshal 缺口只补通用值协议，不加 asset 类型 switch | 通用语言机制 |
| engine/domain/simulation/scripting/core 及 builtin script | 只在有证据需要时补 per-instance 准备/撤销钩子，保留唯一生命周期 | 运行脚本机制 |
| PLAYER 和 Scene runtime 的正式组合点 | 注入同一个 runtime assets provider，关闭时结清 | 正式运行宿主 |
| editor/activities/scene Run 构造路径 | 借运行资产输入，不把 Editor ProjectCapabilities 传到脚本 | Editor Play 接线 |
| cmake/installed-consumers 及相关 tests | 原用例迁移，新增真实无 Editor script/asset consumer | 资格消费者 |
| 生成/安装 CMake、架构 rules、文档 | 删除旧入口和失效规则，新增真实禁止边 | 工程闭包 |

省略号不是允许实施者任意发明路径：R0 必须从 repo 真实路径展开并写入 file-actions。没有可靠路径前不得进行批量文本替换。

## 3. R1：固定计划/准备权限

入口：ProjectPublication 全部消费者已列出。先记录原行为测试与计划可变性编译测试，再实现新类型和原 Operation 的消费。

出口：无 public 可改的 prepared 计划；worker 只能取固定拥有值；同项目第二发布仍被原占用保护；移动/丢弃唯一释放；真实写入后事实保持。所有原调用和测试改为唯一新入口，旧符号不安装。

不顺手改写 VFS、文件替换和 WriteCoordinator 顺序。

## 4. R2：无界面配置准备

入口：声明相关所有权与动态 codec 寿命。先用固定输入构造与原 UI 等价的 draft/正式描述。

出口：无 Root/ImGui 的消费者能够完成原配置/预设/拒绝场景；UI 和非 UI 返回同样的正式配置与错误；Unknown 值不丢；实际 partitioner 输入明确。运行脚本不会因此得到 Editor draft。

## 5. R3：发布活动与正式面板

入口：R1 prepared 可由原发布者消费。迁走 Application 中真正的业务状态，保留泛化的 DerivedArtifact。

出口：无 Application 的正式活动消费者可打包/发布/采用；两个面板能独立构造；内置与外部调用同入口。文件成功但登记失败、源后续保存、关闭中的完成、Unknown 均由唯一活动保持。

不得将全部 Application 字段搬到一个新的 ApplicationService。迁移前后逐项标记“谁拥有、谁只观察”。

## 6. R4：预览、Workspace 和 recipe

三个子工作按独立变化闭包提交：

- 预览：input identity 不含 destination；独立 recipe 与 live owner；两目标共享产物，目标分别采用。
- Workspace：纯 import 函数、明确 IO/状态；原幂等和恢复选中范围保留。
- 模型：recipe codec 与 IO/cook 分开，明确 ModelImport 名称。

出口：没有旧控件/发布/preview 旁路；小型纯算法不新建 Controller；原 Snapshot、History 和 Run 机制不复制。

## 7. R5：运行资产能力与脚本作用域

入口：R0 对 ScriptInstance 生命周期和已有 provider 已作真实核对。

先做无 Lua 的能力实现与测试：真实 loadAsset/AssetReadPort、可运输句柄/结果、完整域/代次、原生 scope owner、预算、显式 release、实例结束与晚到完成。

如果缺少 per-instance provider 释放关联，先在原准备/生命周期补窄钩子。不能完成 Lua 绑定后再以“现在没有 caller_id”为理由留下泄漏或访问任意世界。

出口：C++ consumer 不依赖 Lua/Editor/GPU/LLVM；read/decode/查询不造第二算法；所有接受请求在停止后仍回收，handle 不复活。

## 8. R6：真实 ScriptAbility 和 Lua 投影

使用原 `lux_script_abilities`、semantic traits、LuaValueOperation 与 backend 配置。新叶子只组合能力实现、声明和投影。脚本实例申请的 capability/schema 不匹配时在调用前拒绝。

检查 native static/erased 入口和 Lua 调用的参数、状态码、可恢复错误一致。ordinary asset failure 作为可检查结果，而不是默认 fault 整实例；真正协议错误仍走原错误通道。

出口：真实脚本调用、挂起、恢复、值使用、release 都通过正式 VM/ScriptSystem；不能只调用 C++ dispatch lambda 就声称 Lua 已支持。现有 Delay、同步 Hook、事件、local async 等回归保留，资产不使用 timer 特权。

## 9. R7：正式 PLAYER 与 Editor Run

至少三个运行者：

1. 无 Editor 安装 SDK consumer：原生资产读取 + ScriptSystem/Lua 能力测试。
2. 正式 PLAYER：加载包、运行真实 Lua、请求资产、使用结果、执行一次原命令屏障、正常退出。
3. Editor Run：同一运行逻辑，作者 history/current/dirty 不受运行修改污染；关视图不擅自停止其他 owner。

保留 EC1 骨骼 DLL 的 HEADLESS/WINDOW/APP 三路线，尤其本轮修改 public prepared/preview/能力头后。游戏资产能力不是新的骨骼编辑器，不要求重做 EC1 内容路由。

## 10. R8：代码与工程收敛

- 删除改名后的旧 public 头、alias、旧编译列表、旧安装文件和 unused source。
- 将真实 source provider/target/生成输入对应到架构规则，不给整个 engine 或 script_core 新白名单。
- 修 AGENTS 修改闭包：命名、V 前缀、具名 bool、短路、private 字段、120 列、include 边界、异常/observer。
- 性能检查按本轮新路径：稳定查询、典型 IO/解码、脚本边界、容量和复制；不补旧深链计时。
- 发布质量评价使用函数/责任表、实际依赖和可独立消费者，不用文件数、类数或新 concept 数量。

## 11. R9：最终交付

全量 `all -j 4 -- -k 0`；第二轮无新增工作。修改闭包、PLAYER、SDK、真实脚本/原生与受影响 GPU 顺序运行。最终 `EC2 + STRICT`，实现和证据分别提交。源绑定清楚，不能将旧 SHA 运行成绩记成最终新 SHA 重跑。

报告分四栏：本轮运行通过／准确继承证据／未测或免验／仍未完成。历史 EC1/P13 不重写。

交接必须给出用户应打开的新工作区和 HEAD、原 ProjectBuilder 补丁是否已应用（默认没有）、新旧 ABI 指纹与安装 prefix、删除清单、剩余明确范围。停在 EC2，不自动开启下一阶段。

---

<!-- source: 05_ACCEPTANCE.md -->

# 05　行为验收与工程验证

## 1. 口径

本文件的编号是**验收主题，不是新建可执行文件或测试框架的配额**。优先复用原 CTest/安装消费者与真正的生产实现。验证绑定准确源码、构建与依赖版本。

EC1 报告的 222 回归、13 PLAYER、24 组 SDK、45 公共头只是基线索引；改动以后按真实行为/头/闭包映射。不得为保持数字保留已删除空测试，也不得缩掉断言后凭数量宣称更强。

测试种类分别标记：静态源码检查、编译正/负例、真实 SDK、真实 Process、真实 Lua、PLAYER、GPU/输入。任何一种不得冒充另一种。

## 2. 职责迁移验收

| ID | 操作与输入 | 必须观察到 |
|---|---|---|
| XEC2-01 | 准备失败、重复发布、计划移动构造/赋值、自移动按声明策略 | 不产生半占用；原 publishing 权威唯一；不重复释放、不过早唤醒。 |
| XEC2-02 | worker 读取 plan，owner 放弃未启动或取消已启动发布 | worker 无 ProjectStorage 指针/解除权限；接受任务仍结清；move-only 负例成立。 |
| XEC2-03 | 正确 plan + 错误 receipt/版本/包身份 | 拒绝采用；目录/source/manifest 不被错配数据改写。 |
| XEC2-04 | 文件成功、manifest 失败/Unknown、随后重试/明确放弃 | 已发布事实可查；不重复写已成功文件；不把 Unknown 记失败并回收 lane。 |
| XEC2-05 | 同一配置通过纯 draft 和实际 UI 生成 | 正式编码/系统/绑定/关系相同；纯消费者无 Root/ImGui。 |
| XEC2-06 | 非法 provider/依赖/配置版本、不支持 partition、未知配置 | 原准确错误；无半源/半 UI；unknown bytes/version 保留；不新增此前不支持的 World 编辑声明。 |
| XEC2-07 | GUI/模板/独立插件分别调用配置准备 | 同一实际算法；内置没有私有分支特权。 |
| XEC2-08 | 内置和外部 DerivedArtifact 无窗口发布 | 同一活动；App 不识别具体产物；source/checkpoint/dirty 不被发布改变。 |
| XEC2-09 | 编译后源继续保存，产物晚发布再登记 | 新源保存事实不被旧编译输入倒退；产物保留自身来源。 |
| XEC2-10 | 独立构造 ResultsView/WorkspaceView，销毁一窗后继续活动 | 不含 AppImpl；只观察原结果；UI 消失不销毁其未拥有任务；动作仍走原安全点。 |
| XEC2-11 | 一个编译结果给两个预览，改变一方期望/配方后乱序交付 | 产物不绑定单一 target；两个采用域独立；陈旧结果不能冒充新目标。 |
| XEC2-12 | 默认球体/第二个既有网格配方，失败资源/背压/关闭 | 同一 live preview 算法；最后成功内容保持原版本；资源在真实退休后释放。 |
| XEC2-13 | 请求普通编译失败、预览创建失败、导航临时不可用 | 错误归属准确；不将 preview 错误归 compiler；不丢 native 原因。 |
| XEC2-14 | 查询发布状态、显式刷新布局、多个旧快照、中断重试 | 状态查询不触发目录 IO；selected 恢复范围正确；marker/用户修改/外部版本检查保持。 |
| XEC2-15 | 模型 recipe 编解码、缺依赖、非法路径/摘要、重导入 | 无 IO 的 codec 与真实加载一致；原 ModelCooker 仍唯一；不自动截断或忽略源错误。 |

## 3. 游戏脚本核心验收

| ID | 操作与输入 | 必须观察到 |
|---|---|---|
| XEC2-16 | 纯 C++ 安装消费者走原 AssetReadPort/loadAsset | 真正读取/解码，结果正确；不链接 Editor/Lua/GPU/LLVM。 |
| XEC2-17 | 实际 Lua 通过 ScriptSystem 调 readAsset/typed read | 通过生成/准备/调用/awaitable/恢复整链；不是 C++ 直接调用 callback 冒充脚本。 |
| XEC2-18 | 存在/缺失/类型错误/格式损坏/IO 失败/额度超限 | 分清接纳错误和已接纳业务 outcome；脚本能处理普通 NOT_FOUND 后继续；无异常跨 ABI。 |
| XEC2-19 | 构造大于 2^53 的域/序号，完整 AssetId，非法句柄输入 | 精度不丢；全域/代次/type 校验；不接受伪造 pointer/table/caller。 |
| XEC2-20 | 最小容量 1/2 的并发请求，拒绝后再释放和重试 | 接纳前预算生效；接受工作完成不丢；容量恢复，无静默无限扩容。 |
| XEC2-21 | start 内同步完成与正常后台完成 | 都只走合法完成/恢复点；不在 provider callback 中重入 VM。 |
| XEC2-22 | script stop before IO / after IO before completion / after completion before resume | 三时序资源与额度均结清；无新业务/晚到跨代次采用；不只测 completion.active 一处。 |
| XEC2-23 | 句柄赋值别名、重复 release、槽复用、跨实例访问 | 合同明确；释放失效所有旧别名；不能访问新实例/新结果；没有第二释放。 |
| XEC2-24 | 忘记显式 release 后卸载脚本；满表情况下再创建新实例 | 原生 scope 清理全部结果；不依靠 VM GC/窗口消失；旧域不复活。 |
| XEC2-25 | 已加载数据读取/范围读取：offset+count 溢出、越界、零长度、过大输出 | 返回准确结果；不 IO；拷贝范围和预算明确，不整包 Lua string 化。 |
| XEC2-26 | 资产读完成后 await 原 Delay 再读/写显式开放组件 | 原 ScriptSystem 和 DeferredScriptHost 命令屏障工作；跨 await 不持组件地址；运行修改不进入作者 History。 |
| XEC2-27 | 能力未注入、重复/冲突 provider、错误 schema/type/value codec | 在 prepare/call 对应边界拒绝；缺能力不回落到 Editor 或全局查找。 |
| XEC2-28 | 编译一个新的独立能力贡献，不改通用 backend/core | 复用正常生成/投影；没有 AssetType switch 进入 Lua VM；原 Delay 不被改造为资产特殊路径。 |
| XEC2-29 | 真实插件 codec/value/结果 deleter，脚本停止与最后 owner 释放 | 代码 pin 覆盖最后值析构；非平凡值不冒充 raw resume payload；线程正确。 |
| XEC2-30 | Lua 错误参数/运行错误/正常挂起，typed worker 含可观察 RAII | 继续沿 LuaBoundary；析构完成后才 error/yield；不新增热路径 try/catch。 |
| XEC2-31 | 正式 PLAYER 运行同一资产脚本 | 包/VFS来源正确；无 Editor 编译及运行依赖；正常退出并清空新请求/结果。 |
| XEC2-32 | Editor Run 同一脚本，多视口/关闭一窗/停止 Run | 同一 engine provider；运行与作者分离；没有每个视图一套脚本 owner。 |

XEC2-29 要求的是本轮新增结果/绑定的真实动态寿命；不等于要求任意已加载插件可随时热卸载。若原引擎合同要求阻止或延迟卸载，按原合同验证，而不是强卸代码。

## 4. 工程和性能验收

| ID | 必须执行的检查 | 不可替代的观察 |
|---|---|---|
| XEC2-33 | 原行为映射与 EC1 骨骼 HEADLESS/WINDOW/APP | 删除/修改后原能力仍成立，History/开放路由/恢复/SaveAs 不退化。 |
| XEC2-34 | 真实 CMake/源码/安装负例及对应修复正例 | 在指定错误依赖上拒绝；缺第三方库不能算门禁有效。 |
| XEC2-35 | 公共头与真实 SDK、modules 三 include 前缀、生成输入重建 | 不借旧头/旧 DLL；native headers 不含 Lua 或 Editor；删除入口不留在安装树。 |
| XEC2-36 | 本轮性能/调用计数 | 同一资产不被绑定层再读/再解码；query 无 IO；稳定帧无无条件全目录复制；scope 清理后容量可复用。 |
| XEC2-37 | 全量构建、二次无工作、PLAYER、受影响 GPU/原生输入 | 修改正式消费者后真实运行；不从 build PASS 推导视觉/生命周期通过。 |
| XEC2-38 | 最终源绑定、免验/未测分栏、残留清单、归档 | 记录真实已执行与未执行；不改历史结论，不依赖另一工作树隐藏源码。 |

## 5. 必需依赖负例

至少覆盖下列**真实边**，可放入现有负例框架，不要求新建十四个项目：

1. 游戏资产能力直接或经 INTERFACE/LINK_ONLY 依赖 Editor 的 project/persistence。
2. PLAYER 的脚本运行链含 Editor headers/source/DLL。
3. 纯配置准备包含 SceneConfigurationElement、Pane 或 ImGui。
4. Runtime 资产读取头包含 lua.h 或作者 Session。
5. 通用 ScriptAbility/Lua backend 依赖 Skeleton/Material 的具体资产/编译器。
6. native script assets consumer 必须链接 Lua 才能构建。
7. Lua asset 投影直接打开文件或另有私有线程池。
8. 通用工作台面板包含 ApplicationImpl 或重新开放全量 IApplicationAccess。
9. typed data/component 头 include 行为 system/feature/binding 头。
10. raw memcpy 非平凡对象到外部 async resume（实际编译 negative）。
11. 资产调用使用 PreparedLocalAsyncStart 私有 timer 授权（编译/运行 negative）。
12. 新插件需改变宿主资产 enum 或通用 VM switch 才能接入。
13. 已删除旧公开名仍在 generated/install 对应映射提供。
14. 错误 world/instance generation/capability schema 的调用被接受（真实 runtime negative）。

## 6. 性能证据的最低实用内容

本轮追踪的是新边界，不重做 P10Q 长测：

- 固定依赖/构建模式/输入；同进程 warm path 与 IO path 分开。
- C++ 原读取和 Lua 同调用使用同一 provider；记录实际 read 次数、decode 次数、运输字节与结果保留量。
- Lua 方法准备次数、执行次数、awaitable/continuation 和 native handles 的 high water；复用原 ScriptRuntimeStats/LuaScriptBackendStats。[C16,C20]
- query 方法是否有 IO、taskInfos/snapshot 大数组、每调用字符串方法搜索或重复结构校验。
- 空闲/已取得结果反复查询，未要求绝对零分配；任何新增 per-call heap 必须解释或移至冷准备。
- 小规模与代表性规模各保留可复现样本和算法计数。样本不足不夸大 p99/统计显著；无需固定 100 次。
- 不并发跑 build/安装/性能/GPU。不要把进程启动时间当热调用开销。

容量与生命周期验证是必做，精确 wall-clock 排名不是阶段目的。若没有性能收益也可以保留正确实现，但不能隐瞒显著回退或未结束资源。

## 7. 验证顺序

先原生/纯值/编译负例 → 真实 IO 与脚本 → 安装 SDK/PLAYER → GPU/原生输入 → 最终源与安装残留核对。每批可跑受影响子集，最终以同一实现完成全部本轮适用观察。

全量构建遵守 AGENTS；新增脚本接口需要真实 Lua/ScriptSystem 运行，不要求新增 Linux、IME、Android、sanitizer 全矩阵。若现环境无法进行本轮必需的 PLAYER/Lua 测试，诚实记 PARTIAL，不拿历史免验替代。

---

<!-- source: 06_CONTINUING_RULES_AND_REPORTING.md -->

# 06　持续规则、架构反思与交付格式

## 1. 判断架构清晰，不能只看五个目录

最终文档对每条主要行为画出实际调用链及唯一 owner：

- 资产 read/decode → native result → script handle → resume → release；
- ProjectUpdate → fixed plan → prepared reservation → publication → adoption；
- configuration draft → prepare → Session 的实际领域提交；
- compiled result → target adoption → RenderResources → retirement；
- activity state → read-only view → command/request → original owner。

每条链都回答：数据从哪来、能否修改、谁允许执行、失败是否有副作用、何时失效、谁最后释放。

如果一次普通查询必须穿过 Application→Manager→Adapter→Provider→另一个 Manager，而各层没有独立权限/状态/表示职责，则收敛。若这些层分别承担线程运输、领域准入和资源采用，就保留边界，优化真实数据工作而不是只追求短栈。

## 2. 数据/执行者的命名判据

| 形态 | 正确示意 | 禁止机械修法 |
|---|---|---|
| 纯数据/描述 | `plan.files()`、`snapshot.find(id)` | 给每个 getter 配 Reader。 |
| 纯转换 | `decodeModelRecipe(bytes)`、`prepareSceneConfiguration(draft, descriptors)` | 用只含 static 的 Processor/Operation 空壳。 |
| 执行活动 | `operation.advance()`、`executor.undo(history)` | 两套同义状态机并存。 |
| 资源拥有 | `host.adopt(view)`、`scope.requestStop()` | 把权限和生命周期拆成外部可改 bool。 |
| 语言表示 | `makeScriptAbilityLuaContribution<Ability>()` | 在 Lua callback 中复制 native 算法或生成任意方法反射总线。 |

EC1 的 History 分离是已落实的具体决定，不是要求所有 State/Store 都变成 POD。真正带 invariant 的数据允许 private storage、构造/观察及必要 RAII。

## 3. 用类型减少冗余判断，但保留真实边界

每个删除/合并检查条目包含：首次证明点、复用范围、可能失效事件、公开/私有入口、替代类型和回归。

可以收敛：同一个 immutable plan 的多次字段组合校验；无回调的同步段反复 find；同一版本稳定目录多消费者复制。

必须保留：外部脚本值 decode，能力 schema 与绑定身份，跨 await 的实例/资源代次，外部文件发布前版本，codec/插件回调后的状态，旧完成对新目标的采用验证。

没有 callback/await 不自动保证线程安全，仍按具体 owner 合同。`completion.active()` 是事实查询，不是锁，也不保留未来有效性。

## 4. C++20 和静/动态绑定

- 继续 `lux::cxx::expected`，不引入未声明的 C++23 标准库设施。
- concept 用于 codec/输入布局/typed completion/真实共享算法的语义表达；不能证明运行期有效域、原子提交或数据深不可变。
- 运行时开放的插件/能力在已有边界擦除一次；不能用封闭 variant 冒充未来所有扩展。
- 数据结果封装关联；可复制只能复制不可变共享数据，不能复制取消/发布/退休责任。
- 固定地址对象禁止复制/按值移动，拥有单元 move 后原对象无重复释放。资源值替换先清旧 payload 再清 code。
- 限定模板在真正使用处实例化；不把整个 EditorApplication/Runtime 模板化。
- `function_ref` 只在当前同步栈借用；异步或存入对象的闭包用合适 owning callable，并明确捕获寿命。

## 5. AGENTS 对照（修改闭包内必核）

| 规则 | 实施检查 |
|---|---|
| 120 列/语义换行/4 空格 | 不用过度机械拆行制造多层噪音；长返回值先给准确别名。 |
| PascalCase / E / V / private 尾下划线 | 新/修改 public 类型及 intent/value 一致；不再出现 Data 名掩盖执行。 |
| 具名 bool 与短路 | 独立条件按语义组命名；指针/范围/算术先建立安全前提。 |
| include/sinclude/pinclude | 外部插件只有真实安装契约；不拿私有路径充当 SDK。 |
| 数据组件只包含数据依赖 | Script/Feature/系统行为不进入组件/资产描述头；共同编码归原下游值。 |
| 异常 | 语义失败用 Result；仅规定的 foreign/factory/codec 容纳边界处理异常；不向 dispatch/drain 插 catch。 |
| Lua 错误/yield | 沿原 C boundary，不跨未析构 C++ owner；不以 noexcept 当作不会失败。 |
| Observer | 连信号折入存量；结构修改在原命令屏障；嵌套入队留后批；需通知的字段通过 patch。 |
| 诊断 | 结构化错误保留；库不决定终端出口，单一宿主 sink。 |
| 构建 | all/-j4/-k0、两轮无工作、无并发实机；真实 provider 声明；模块头三 prefix 同步。 |
| 删除 | 旧入口/alias/generated/install 残留一起清；只读旧资产迁移和历史证据保留。 |

标准 Lua 手册只作为 C API 边界背景；本项目实际 Lua55 补丁、ABI、semantic 与 generated binding 才是执行依据。不要联网更新依赖或更改 Lua 版本来绕开本轮问题。

## 6. 架构上的“保留”也必须写进报告

以下不需要成为新全局类型：ScriptManager、GameApiContext、UniversalResourceHandleStore、EditorServiceLocator、ProjectPublisher2、PreviewCoordinator、WorkspaceWriter、ConfigurationProcessorFactory。

`ScriptAssetScope`（目标）若确实新增，其责任只限 **此原生脚本实例的已接受请求和结果保管**，不是系统级缓存、线程池或未来万能所有者。能复用现有实例资源域就不另建。

编译错误与预览错误、作者源与 cooked bytes、文件效果与目录采用、能力可用与即时准入，继续明确区分。这些区别不是冗余。

## 7. ABI 与版本

- EC1 Editor V8 保持为当前事实，不回写 V7。Editor public 类型改变才更新实际 ABI 指纹及对应消费者。
- 新 script method/value schema 为独立稳定版本。加入普通方法贡献不等于需要更改通用 VM ABI。
- 如果补 per-instance prepare/revoke 的原合同需要变更现有结构布局，更新真正受影响的 Script ABI 并证明旧输入在调用前拒绝；不要一并无故改 Runtime 插件 ABI。
- 项目 manifest v3/旧版本只读迁移保持；此次职责重分配默认不改变磁盘格式。
- 稳定 ID 的规范名/哈希/版本不因 C++ 文件移动擅自改变。

## 8. 每批交接字段

在现有账本 ec2 节点记录，不创建第二账本。至少包括：

```text
batch / input_sha / current_sha / dirty_status
source_files_and_symbols
responsibility_before -> responsibility_after
removed_entries / retained_entries_and_reason
owner_and_lifetime_changes
native_and_script_contracts
invalid_states_eliminated
checks_removed_and_validity_proof
tests_executed_with_commands_and_exit_codes
inherited_evidence_with_original_sha
not_run_or_waived
next_entry_and_known_blocks
```

机器清单可用本包 [IMPLEMENTATION_MAP.json](IMPLEMENTATION_MAP.json) 作为种子；它不是已完成文件清单，R0 必须补全真实消费者。

## 9. 最终验收记录格式

```text
stage: EC2
implementation_sha: <完整最终实现>
acceptance_sha: <独立记录提交>
baseline: 248adc4576943cab83976afd8d1d5f31b63b70a9
status: PASS_WITH_DECLARED_SCOPE / PARTIAL （按实际决定）

A. EC1 继承：已复核/未重跑，准确 SHA
B. 职责迁移：原算法、唯一新位置、旧入口删除
C. 游戏脚本：实际 exported capabilities 与明确未绑定项
D. C++ / Lua / PLAYER / Editor Run 真实链路
E. 依赖、生成、安装、AGENTS 检查
F. 性能与生命周期：计数/样本/覆盖范围
G. 免验、NOT_RUN、未完成项
H. 工作区、用户补丁、main/分支/发布状态
```

不得用一句“全绿”覆盖 A–H；也不需要为保留范围建立大量额外胶水文件。

## 10. 施工结束的判据

真正可复用能力不再被控件或 AppImpl 私藏；固定数据不夹带隐含可变权限；C++ 与脚本共享真实执行，脚本边界有完整寿命；正式产品和 PLAYER 可运行；旧入口实际退出。

达到这些目标后停下复审，不因为还能设想更多 API 就再扩展阶段。未来物理/音频等完整绑定按具体用户需求推进，不在本轮自动追加。

---

<!-- source: 07_SOURCE_INDEX.md -->

# 07　固定来源、事实与执行边界

## 1. 本次实际做了什么

核对当前 Git ref/commit，读取 EC1 报告、上一轮职责调查和 AGENTS，并读取下列代表性头/实现。没有 clone/编译整个项目，没有运行 EC1/ScriptSystem/Lua/GPU/PLAYER 或完整归档验证。也没有根据无法检索的巨大树推断某模块不存在。

本包是施工设计，不是 EC1 PASS 收据。新类型/API/叶子位置均为目标设计。凡只是旧调查的发现而没有本轮重读完整实现者，R0 必须复核当前消费者和剩余责任。

固定代码 HEAD：`248adc4576943cab83976afd8d1d5f31b63b70a9`。源码引用为固定 GitHub blob URL，不随分支以后移动。API ref URL 只用于本次定位，不作为未来固定代码。

## 2. 当前代码与报告

| ID | 来源 | 本次读取与用途 |
|---|---|---|
| C01 | [分支 ref](https://api.github.com/repos/LUX-YU/lux-engine/git/ref/heads/codex/editor-redesign-v4) | 2026-10-03 返回 248adc4576943cab83976afd8d1d5f31b63b70a9 |
| C02 | [验收提交与实现父提交](https://api.github.com/repos/LUX-YU/lux-engine/git/commits/248adc4576943cab83976afd8d1d5f31b63b70a9) | parent 137b8f441faa1dbb7dfa792b6f01b513327e6248 |
| C03 | [dev_log/EC1/report.md](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/dev_log/EC1/report.md) | 完整报告；实施方证据摘要，不是本次独立运行 |
| C04 | [editor/application/src/EditorArtifacts.cpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/application/src/EditorArtifacts.cpp) | 1–155；新 DerivedArtifact 输入及仍在 Application 的执行段 |
| C05 | [editor/activities/project/include/lux/engine/editor/storage/ProjectPublication.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/activities/project/include/lux/engine/editor/storage/ProjectPublication.hpp) | 完整头；固定字段与 owner_ 混合仍在 |
| C06 | [editor/activities/material/include/lux/engine/editor/material/MaterialPreviewStore.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/activities/material/include/lux/engine/editor/material/MaterialPreviewStore.hpp) | 完整头；任务输入和 live preview 接口 |
| C07 | [editor/workbench/scene/include/lux/engine/editor/scene/SceneConfigurationElement.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/workbench/scene/include/lux/engine/editor/scene/SceneConfigurationElement.hpp) | 完整头；控件、正式配置值和 builders 依赖 |
| C08 | [editor/workbench/scene/src/SceneConfigurationElement.cpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/workbench/scene/src/SceneConfigurationElement.cpp) | 540–725；当前 applicability、preset、systemOptions 起点 |
| C09 | [editor/application/src/EditorResults.cpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/application/src/EditorResults.cpp) | 105–185；VResultIntent 已使用，ResultsPane 仍借 AppImpl |
| C10 | [editor/activities/material/include/lux/engine/editor/material/MaterialCompilation.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/activities/material/include/lux/engine/editor/material/MaterialCompilation.hpp) | 完整头；key.target、CompiledMaterial 与 preview error 域 |
| C11 | [editor/activities/workspace/include/lux/engine/editor/workspace/WorkspaceStore.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/activities/workspace/include/lux/engine/editor/workspace/WorkspaceStore.hpp) | 完整头；query/IO/policy/migration 接口 |
| C12 | [modules/function/script/core/include/lux/engine/function/script/ScriptAbility.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/modules/function/script/core/include/lux/engine/function/script/ScriptAbility.hpp) | 1–250；typed/erased completion、方法、值寿命；不是全文件穷尽阅读 |
| C13 | [engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp) | 完整头；外部结果类型限制与原 awaitable/start 流程 |
| C14 | [engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptLocalAsync.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptLocalAsync.hpp) | 完整头；ScriptTimers 私有资格 |
| C15 | [engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptApiCapability.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptApiCapability.hpp) | 完整头；publication/prepared capability 形状 |
| C16 | [engine/domain/simulation/scripting/lua/include/lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/lua/include/lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp) | 完整头；config/values/abilities/stats |
| C17 | [modules/function/script/lua/include/lux/engine/function/script/lua/ScriptAbilityLua.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/modules/function/script/lua/include/lux/engine/function/script/lua/ScriptAbilityLua.hpp) | 完整头；value operation 与 generated traits 入口 |
| C18 | [engine/process/asset_loading/include/lux/engine/process/asset_loading/AssetLoadSender.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/process/asset_loading/include/lux/engine/process/asset_loading/AssetLoadSender.hpp) | 完整主要头 1–260；read sender/typed decode/cancel |
| C19 | [engine/process/asset_loading/include/lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/process/asset_loading/include/lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp) | 完整头；请求保活/TaskScope/stop/join |
| C20 | [engine/domain/simulation/builtin_systems/script/include/lux/engine/simulation/ScriptSystem.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/builtin_systems/script/include/lux/engine/simulation/ScriptSystem.hpp) | 1–355；runtime budget/stats/create/region/lifecycle |
| C21 | [engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/DeferredScriptHost.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/DeferredScriptHost.hpp) | 完整头；显式 component 绑定与命令写入区域 |
| C22 | [engine/domain/simulation/builtin_systems/script/include/lux/engine/simulation/abilities/DelayAbility.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/builtin_systems/script/include/lux/engine/simulation/abilities/DelayAbility.hpp) | 完整头；实际注解式能力声明 |
| C23 | [engine/domain/simulation/builtin_systems/script/CMakeLists.txt](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/builtin_systems/script/CMakeLists.txt) | 完整；lux_script_abilities 真实生成与安装 |
| C24 | [engine/process/README.md](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/process/README.md) | 完整；Process 职责；旧编辑器链接文字不作为当前目录证据 |
| C25 | [dev_log/EC1/S2.md](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/dev_log/EC1/S2.md) | 完整；Editor V8 三类能力组合的实施方说明 |
| C26 | [modules/function/script/lua/include/lux/engine/function/script/lua/LuaBoundary.h](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/modules/function/script/lua/include/lux/engine/function/script/lua/LuaBoundary.h) | 完整；typed worker 正常析构后 C error/suspend |
| C27 | [engine/process/asset_loading/src/VfsAssetReadEndpoint.cpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/process/asset_loading/src/VfsAssetReadEndpoint.cpp) | 1–170；读取数量准入及 vfs.open 调用；未审完下游 provider 内存边界 |
| C28 | [engine/domain/simulation/scripting/lua/CMakeLists.txt](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/lua/CMakeLists.txt) | 完整；native/Lua/runtime 的实际依赖 |
| C29 | [engine/domain/simulation/scripting/CMakeLists.txt](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/CMakeLists.txt) | 完整；backend 分组与当前非 Android Lua 条件 |
| C30 | [engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptEndpointBridge.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptEndpointBridge.hpp) | 1–230；Hook/Event 描述与回调/值复制协议 |

## 3. 用户材料与外部背景

| ID | 来源 | 用法 |
|---|---|---|
| U01 | [用户 AGENTS 原件](references/AGENTS.user.md)；当前仓库根 AGENTS 同一 LF Git blob `1fd3123156dc7f59f98cbcd422f227a16352cddc` | 风格、依赖、异常、observer、构建/同步。复制原字节，不因本包意见修改。 |
| U02 | [上次职责调查原件](references/PRIOR_RESPONSIBILITY_AUDIT_54f8a563.md) | 旧基线 `54f8a563…` 的调查。EC1 已改事项由 C03 和新源码纠正；“并入 EC1”不再执行。 |
| W01 | [Lua 5.5 Reference Manual](https://www.lua.org/manual/5.5/manual.html#4.4)，§4.4、§4.5、userdata 相关定义 | 只用于解释非局部错误/yield 和 continuation；实际绑定仍以本仓库 Lua55 和 LuaBoundary 为准，不据此升级依赖。 |

## 4. 证据分级

**当前源码事实**：例如 EditorArtifacts 已用 DerivedArtifact、Preview 仍用 CompileOperation、外部 async 结果限制、LuaBoundary 合同。

**实施方报告事实**：EC1 测试数量、骨骼模式、user patch/工作树、性能样本和删除结果。它们是报告内容，本次未独立重跑。

**设计裁定**：EC2 的职责目标、API/作用域/目录建议、测试矩阵。不能把这些未来要求说成已经完成。

**待执行核对**：全部实际消费者、逐实例 capability 的完整内部链、VFS provider 的读取前字节限制、完整安装/target 闭包。提供者已有等价实现时复用，不重复建设。

## 5. 文档本地检查不是引擎资格

本包只进行了文件生成、链接/引用/代码围栏/JSON/压缩包校验、用户参考字节比对。没有生成伪运行结果。`PACKAGE_CHECKS.json` 描述这些本地文件检查，不进入工程 PASS 计数。
