# EC1 补充调查：除 History 外的类型职责与边界收敛

**日期：2026-10-02**  
**仓库：LUX-YU/lux-engine**  
**固定审阅提交：`54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8`**  
**性质：源码责任调查与设计建议；不是实现提交、阶段验收或新增强制阶段。**

## 0. 结论

还有，而且其中若干问题比 `history.undo()` 的主语更值得优先处理。但不能据此将所有 Store、Snapshot、Session、Operation 都拆成 Data/Executor/Result。

本次最重要的发现是：

1. **项目发布描述与发布占用权混在 `ProjectPublication` 中。** 对象看似可编辑的数据包，析构却会解除 ProjectStorage 的发布占用。
2. **场景配置的草稿、领域编译、预设和 UI 被放进 `SceneConfigurationElement`。** 想在没有 Root 的脚本／插件中生成同一配置，会遇到 UI 依赖。
3. **`ArtifactPresentation` 并不是普通呈现记录。** 它和 `settleArtifacts()` 实际承担一整条打包、文件发布、项目目录采用流程。
4. **WorkspacePane、ResultsPane 直接读写 Application::Impl。** 它们虽使用了正式 Pane，却没有使用与外部工具相同粒度的依赖契约。
5. **`MaterialPreviewStore` 是一个活动预览，而不是记录仓库。** 它还内置球体、场景配方、Feature 选择，并直接消费编译操作对象。
6. **WorkspaceStore 同时提供文件存储、布局回退政策、旧格式转换和迁移推进。** 其中 `layoutResult()` 还把廉价状态查询与全目录 IO 绑定在一起。

另外，`AssetImporter` 的领域命名与配方解析、`ProjectOpenData` 的独占资源语义，以及已列入 EC1 的 Snapshot、Intent、SessionPreparation，都应按不同强度整理。

**建议保持五层，不新增万能管理器，不复制引擎设施。将真正能独立复用的算法迁到正确提供者，将数据关联收紧，将实际的资源 owner 保留下来。**

---

## 1. 调查依据、范围与证据边界

本轮直接检查了 Editor 的配置 UI、工作区、项目发布、导入、编译预览、结果 UI、会话控制，以及相邻的 ModelCooker、SceneDescription 和渲染职责说明。对主要发现同时核对公开声明、关键实现和至少一条真实消费链。

来源编号 `[Rxx]` 对应文末与 `SOURCE_INDEX.json`，均绑定同一 SHA。少量已在上一轮完整读取的类型，本轮通过其消费者继续验证，并明确标为“EC1 已知／本次深化”。上传的 AGENTS 与 EC1 是用户要求和前置设计，不冒充已经实施的新代码。

证据分级：

- **代码事实**：当前定义和实际调用直接支持。
- **设计判断**：基于细粒度复用、数据／执行职责和插件开放目标，提出更合适的划分。
- **潜在后果**：由静态结构推导，尚无实际运行负例，不表述为已经损坏数据或发生崩溃。
- **目标名称／接口**：以下新名称都是建议，不是当前存在的 API。

本次没有运行引擎、SDK、GPU、DLL、性能基准或全仓 AST 检查，不声称已经覆盖全部类型。没有修改远端、main、用户补丁或历史收据。

## 2. 判断职责的标准

### 2.1 五种不同的对象

| 类别 | 主要内容 | 合理行为 | 不应顺手承担 |
|---|---|---|---|
| 描述／草稿／值 | 内容、配置、标识、关系 | 构造、查询、维护自身不变量 | 调度任务、操纵窗口、控制外部生命周期 |
| 快照／只读观察 | 固定版本及其 owner | 无歧义查询、同版本索引 | 允许调用者将观察字段改成另一份数据 |
| 算法／编译器 | 转换与验证规则 | 处理输入并返回结果 | 保存第二份长期 current/dirty 或内容源 |
| 活动／资源 owner | 占用权、任务、场景实例、在途结果 | 准入、推进、取消、退休、确认 | 因“数据纯化”将所有权散落在外部 |
| UI／呈现器 | 显示缓存、控件、手势 | 展示、编辑草稿、发出准确意图 | 决定文件发布协议、持有整个 Application 私有状态 |

### 2.2 不是所有有方法的数据都要拆

数据可以有查询、构造、索引和自身结构维护。`description.findSystem()` 并没有替外部世界执行业务。`Snapshot::create()` 如果只是构造合法不可变值，也不必再加 SnapshotCompiler。

资源 owner 同样可以有行为。`ViewHost.adopt()`、`TaskScope.submit()`、`SessionState.prepareClose()` 的主语与责任可以一致。

真正需要分开的信号是：变化理由不同、执行环境不同、调用者集合不同、错误域不同，或者纯处理不得不依赖 UI/文件/活动对象。

### 2.3 删除转发不等于删除边界

有价值的改变应当减少实际算法重复、隐含权限和不正确的依赖。以下不算职责重分配：

```cpp
Result NewProcessor::process(OldType& value)
{
    return value.process(); // 算法、数据和权限仍原封不动地捆在一起。
}
```

同时，不能为消除这一行转发，让插件直接改 Registry、清 WriteCoordinator 的占用或在 UI 回调删除 Pane。

---

## 3. 高优先级一：ProjectPublication —— 数据计划与独占权限

### 3.1 当前事实

`ProjectPublication` 的 public 字段包含 root、manifest_path、before_manifest_digest、manifest、manifest_bytes、files、package_paths，看起来是一份发布计划。[R03]

它同时持有 private `ProjectStorage* owner_`。析构会：

```cpp
owner_->publishing_ = false;
for (const auto& waiter : owner_->publication_waiters_)
    waiter.request();
owner_->publication_waiters_.clear();
```

`ProjectStorage::preparePublication()` 验证并取得这项占用；`adoptPublication()` 检查 owner、receipt 并采用新目录。[R04]

实际 `ProjectPublicationOperation` 已经存在。它拥有上述对象、任务、FILE/PACKAGES/MANIFEST/ADOPT 阶段和回执，并在工作线程只捕获需要的文件／清单数据。[R05] 本次没有发现 worker 正在直接释放这个占用，不能混淆“类型允许混合”与“实际线程误用”。

### 3.2 问题不是析构有行为，而是两种契约没有表达清楚

- 发布计划应是固定、可交给 worker 读取的输入。
- 发布占用是 owner-thread 的独占权限，销毁会改变另一个 owner 的可用性。
- 当前准备完成后，公开字段仍可被改写；manifest 与 manifest_bytes 等对应关系没有由类型封住。
- 同一对象同时像 DTO、prepared token、资源 lease，未来调用者很难只凭类型知道能否修改、复制或转移。

没有证据显示现有生产调用已经伪造字段或造成磁盘错误。本条是可组合公共 API 的责任缺口。

### 3.3 目标关系

```text
ProjectUpdate（请求值）
    → 纯验证／构造候选
    → ProjectPublicationPlan（固定文件与清单）

ProjectStorage（真实项目 owner）
    → PreparedProjectPublication（plan + 唯一占用）
    → 原 ProjectPublicationOperation 执行
    → ProjectPublicationReceipt
    → ProjectStorage 采用
```

可以将现有 `ProjectPublication` 重命名并封装为 `PreparedProjectPublication`，内部持一份 plan；不要求新增独立堆对象。也可以将 reservation 放入已有 Operation，前提是准备、失败返回和调用方尚未启动 Operation 的寿命仍完整。

**不能**新增另一套 ProjectWriteCoordinator；不能把所有数据字段 public 后要求每个消费者重复验证；不能把 live ProjectStorage 交给 worker。

### 3.4 精确迁移与保留

迁出／替换：现有 publication 的 public 可变计划字段、隐含的 prepared 权限命名。

保留：原 publishing_ 权威、waiter 唤醒机制、manifest 版本、immutable 文件复用、写入后的结果事实、跨回调重新验证和正确释放顺序。

验证：准备失败无半占用；移动后唯一释放；放弃时已发布文件事实保留；worker 只拥有固定输入；同项目第二次发布仍等待原 owner；目录采用不能接受不同计划的 receipt。

## 4. 高优先级二：SceneConfigurationElement —— 表单不应是唯一配置编译器

### 4.1 当前事实

同一个 CPP 定义了 UI 控件和大量内容构造逻辑：[R01]

- `ConfigurationField` 处理原始配置字节、默认 codec、控件编码。
- `SystemElement` 编码 Transform／WorldLoading／Render 配置、绑定 provider，并选择预设 Feature。
- `Impl::build()` 读取 checkbox/choice/text 控件，建立 SimulationDescriptionBuilder 和 SceneDescriptionBuilder，写入 construction/execution/channel/dependency 关系，最后返回正式配置。
- `load()` 根据实际资产恢复控件、系统行与关系。
- `systemOptions()` 调 `catalog.systemsForWorld(..., "lux.spatial.builtin.single")`，没有使用正在编辑的 World 的实际 partitioner。
- `SceneCreationView` 的创建流程消费 `form_.build()` 的结果。[R02]

单分区筛选这一代码事实不能被扩大为“所有索引 World 都已产生错误”；当前源本身也有明确范围限制。但是它说明世界能力政策藏在控件内部。

### 4.2 目标关系

```text
SceneConfigurationDraft（拥有的纯选择、配置字节、关系）
    → prepareSceneConfiguration(draft, 固定描述/能力)
    → 既有 Scene/Simulation Builders
    → 正式 SceneConfiguration 或 SceneSetConfiguration

SceneConfigurationElement
    仅负责显示、草稿输入、控件结束编辑和诊断
```

目标不需要同时新增 SceneConfigurationCompiler、Assembler、Manager、Validator 四个类。没有长期状态时，一个具名纯算法即可。

草稿属于 authoring/scene；仅依赖纯描述的构造算法亦可在该层。实际注册、Feature 默认值和提供者解析放 activities/scene 的具体准备函数，保持 authoring 的 CPU/无 UI 闭包。workbench 负责将控件变更转换为草稿，不携带运行中的 Renderer。

动态配置控件可能仍需局部 typed scratch。Apply 时经既有 codec 捕获成拥有字节；不再让控件树成为唯一可读取的领域配置。未知 payload、code owner 和原始版本必须保留。

### 4.3 实际收益与验收

同一配置可以由 GUI、脚本、测试、插件或项目模板构造，不要求先建立 Root。2D/3D、World 分区和 Feature 适用性使用同一准备规则；不是将整个场景硬压成一个 bool。

迁出 `SystemElement` 的领域编码/依赖与预设规则，保留它的布局、控件和显示；`load()` 拆为纯解码到草稿与草稿安装到控件两步。原引擎 builder 继续执行正式校验，不复制第二份依赖解析算法。

验证纯输入与 UI 输入的编码一致；错误 provider/版本/依赖不改变源；未知值往返；当前实际 partitioner 用于候选判定；失败保持旧表单及其 code owner。

## 5. 高优先级三：ArtifactPresentation —— 它是业务流程，不是只读显示记录

### 5.1 当前事实

`EditorArtifacts.cpp::settleArtifacts()` 实际执行：[R06]

1. 检查作者版本、绑定与源身份。
2. 决定 cooked_path，解析真实文件目标并 reserve。
3. 在 Process 中分别识别 CompiledMaterial/CompiledFlow，重新编码源以取得摘要，生成 pak。
4. 交编码结果、等待文件发布和 Unknown。
5. 读取已发布 package 的目录。
6. 准备、编码并发布新的项目 manifest。
7. 构造 adoption receipt，采用项目目录并确认票据。

`ArtifactPresentation` 保存 encoding/reading/settled、多组 optional、compiled source、ticket、package、catalog 和 failure。这不是 presentation-only 数据。

### 5.2 目标划分

- 具体编译器／codec：产物和对应源指纹。
- 纯打包算法：将拥有的产物描述变成包字节，不知道 UI。
- 项目发布活动：唯一在途状态、文件票据、目录采用与结果。
- Application：选择用户目标，发起或观察活动，协调关闭。
- ResultsView：只观察结果和发出动作。

先复用 [R05] 已有 `ProjectPublicationOperation`。它已经有文件→package→manifest→adopt 流程，不能再复制一份 ArtifactPublisher 状态机。

但也不能未经验证就简单用现有 Operation 替换：现有路径有“文件先发布、项目登记后来失败”的真实事实；源保存可能在编译期间推进；Unknown 和已接纳后关闭必须继续保留。只补原执行者必要的“已准备包／已有文件”入口，或将确实独立的领域步骤提取一次。

### 5.3 可读性改变

`ArtifactPresentation` 中属于在途执行的成员应归真实活动记录；呈现值改为只读结果摘要。状态互斥用少量命名状态表达，不要求每个 bool 都变成一个独立类。

对编译输入只在异构边界擦除一次。产品不再通过 closed variant 判断是 Material 还是 Flow，插件的其他产物也能复用同一发布能力。

### 5.4 验证

无窗口也能发布；内置与外部产物都不需修改 Application；源摘要来自冻结输入；源稍后保存不被倒退；文件成功但目录失败仍可查询并处理；确认不丢失在途责任；视图关闭不销毁未拥有的发布操作。

## 6. 高优先级四：WorkspacePane / ResultsPane —— 内置 UI 不应享有私有捷径

### 6.1 本次新增确认

`installWorkspaceView()` 在 Application CPP 里定义本地 `WorkspacePane`，内部 `Content` 直接持有 `EditorApplication::Impl&`，读取迁移、恢复、目录、发布结果并设置 workspace_intent_。[R07]

`installResultView()` 同样定义本地 `ResultsPane`。draw 直接访问 sessions_、writes_、runs_、artifacts_、save_reports_ 等，并写入 result_intent_。[R08]

两者没有私自提交文件或在 draw 删除窗口，这是正确的。但“只记录意图”并不等于依赖已经足够细粒度。

### 6.2 应如何分配

- 真实 Workspace/Save/Run/Import 活动继续拥有自己的事实。
- 工作台面板通过只读观察和已经存在的命令／请求契约消费它们。
- 应用负责组合面板所需引用和操作，但不把整个 Impl 交给面板。
- 结果显示所需的跨域摘要可以是只读聚合，不得成为第二个任务或结果权威。

不必把所有产品专用 UI 都强行迁到公共 SDK。真正只适合一个产品的胶水可留 application；**但这两个具有通用工具意义的面板，至少应能在没有 Application 私有头的情况下构造和测试。**

不能只改为 `IApplicationAccess` 再暴露全部 getter；不能造一个 mutable SharedWorkspaceState 给多个组件共同写。

### 6.3 性能与数据形状

ResultsPane 的 draw 当前逐次 snapshotIds/describe 并查询多个结果。这是实际调用路径，尚无本轮耗时测量。先记录调用数，再决定是否需要按 revision 共享一个只读显示快照，避免每个帧／面板各自复制。

新增“窗口只读数据＋动作”接口应足够具体，同时允许插件直接使用更底层的资产和 Pane 能力；不能要求所有扩展都先成为 ResultsPane 的一行。

## 7. 高优先级五：MaterialPreviewStore —— 活预览、内容配方、编译结果消费分开

### 7.1 当前事实

这个类的公开注释明确说“一目标一 store”；它公开 navigate、instance、camera、close，并持有 SceneInstanceLease 和退休回执。[R12,R13]

同一实现还硬编码：

- 48 slices、24 rings 的球体网格与编码；
- 单分区预览 World；
- 六个固定 Feature 名称；
- 相机、灯光与初始变换；
- 编译结果 desired/prepared/accepted；
- 资产 overlay、资源失败恢复与实例退休。

`receive()` 接收的是 `MaterialCompileOperation&`，先检查 ready，再读取 key/result。[R13] 这把预览入口与某一个任务控制类型耦合起来。

### 7.2 目标不是把资源 owner 变成 POD

建议现有类收敛为 `MaterialPreview` 这一活动 owner；名字可以保持现名直至相关消费者同批迁移，但最终应准确反映单目标的活预览。

- 预览配方：独立的纯值／既有 ScenePackage，表达球体或其他模型、相机灯光和 Feature 需求。
- 配方构造：纯函数或现有 builder，不能再实现 Renderer。
- 活预览：唯一实例、候选／已采用资源、版本、相机操作与退休，仍封装在一个对象中。
- 编译完成：入口接受拥有型编译结果和原始请求键，不必要求拿到 operation 控制对象。

可以保留一个默认球体配方。选择自定义网格不应该要求修改预览控制状态机；但是本轮不必交付通用预览脚本语言或全部背景编辑 UI。

### 7.3 CompiledMaterial 的两个身份域

当前 `MaterialCompileKey` 同时含 content/settings/environment 与 target，并被放入 `CompiledMaterial`。[R14]

- content/settings/environment 描述编译输入。
- target 描述这次完成准备送到哪个预览目标。

建议将编译输入键与采用目标绑定分开。不是去掉迟到结果保护，而是让同一不可变产物可以在两个预览中分别验证采用，不需篡改产物本身的 target。

这条是设计判断，不是已经测得“两窗口必然重复编译”的运行结论。与内容无关的 target 不应成为纯编译结果的本体属性。

### 7.4 错误域也应按责任归位

当前 `VMaterialCompileFailure` 包含 `MaterialPreviewFailure`，preview 的 reset/navigate 返回 `MaterialCompileResult`。[R12,R14] 导航失败不是编译失败。

编译诊断与预览/资源诊断应分别由实际模块定义；工具 UI 可使用自己的汇总 variant。不要再创建通用 ErrorAdapter，也不能把所有底层错误压成 BUSY。

### 7.5 必须保留

编译 operation 唯一控制责任、过期结果不采用、最后成功图像原版本、真实资源就绪、背压候选、Runtime 唯一实例与退休、两视口不共享错误 namespace，以及代码／payload 的正确析构顺序。

## 8. 中高优先级：WorkspaceStore —— 存储、政策、迁移与状态查询

### 8.1 合理的现有责任

Store 负责定位版本化文件、读写、列目录并不奇怪。它确实复用原 WriteCoordinator，不应另造 WorkspaceWriter。[R09,R10]

### 8.2 应移出的两种规则

`chooseLayout()` 同时读取布局并决定哪些错误允许回退；这是“选择政策＋读取”。可以保留一个便捷业务入口，但纯选择／错误分类规则应能从已取得的输入独立测试。

`prepareLegacyMigration()` 同时遍历文件、读字节、解析 TOML/INI、选 recovery 来源、生成稳定 ID 和摘要；`continueMigration()` 每次又调用该函数再全量解析，之后检查已发布项并接纳至多一项写入。[R11]

目标：

```text
读取并固定旧源字节/版本
    → 纯 importLegacyWorkspace / planLegacyMigration
    → LegacyMigrationPlan
    → 原协调器支撑的迁移推进
```

继续使用真实盘上状态和源指纹，不引入 private done bit。只可以在证明输入版本不变后复用解析结果；不能删除对外部文件变化的检查。

### 8.3 layoutResult() 不是廉价的“取回执”

当前实现无条件执行：

```cpp
return LayoutCommitReceipt{coordinator_.status(ticket), listLayouts()};
```

实际代码先检查 status 成功，然后构造同样的结果。status 未限定 TERMINAL；listLayouts 会枚举并读取所有布局。[R10]

因此：

- `LayoutCommitReceipt` 名称暗示已经提交，但内部可以是 pending WriteStatus。
- 一次结果查询隐含全目录 IO。
- 文件发布事实与目录刷新结果虽存成两个字段，调用契约仍把两种成本捆在一起。

应分开 `writeStatus(ticket)` 与显式 `refreshLayouts()`，或者将合并结果准确命名为“状态与目录观察”，且明确其 IO 成本。真正 CommitReceipt 仍由原协调器提供。

**当前正式应用没有每帧调用 layoutResult。** 它使用 writes_.status，在 terminal 后才 listLayouts。[R07] 因此这不是已证实的每帧全目录热点，而是公共 API 与实际好用法不一致。

## 9. 中优先级：AssetImporter —— 模型导入活动与配方 codec

### 9.1 已有良好分离不能推翻

`engine/toolchain/.../ModelCooker.hpp` 已经提供：[R20]

```text
ModelSource + ModelCookConfiguration
    → cookModel
    → ModelCookProduct / ModelSourceRequests / ModelCookFailure
```

CPU cook 不直接读取文件，缺依赖由 blocking 读取后补入。另有磁盘便利重载，并明确区分。它已经符合用户认可的数据／算法／结果形态，不需要再包 ModelCompilerExecutor。

### 9.2 Editor 侧的职责仍可收敛

`AssetImporter` 的公开请求只有 requestModel/reimportModel，成功值也是 ModelAsset。[R18] 这是模型导入活动，不是开放的“所有资产导入器”。

其 CPP 中 `Load` 同时读 `.luxmodel` recipe、解析 TOML、校验路径/digest、读取捕获文件；`Encode` 构造 recipe、打包 cooked 资产并产生 ProjectUpdate。[R19]

建议：

- 明确当前名称是 ModelImporter/ModelImportActivity，或在文档上承认仅模型能力，不预先造万能导入注册器。
- 将真正可复用的 `ModelImportRecipe` 及 decode/encode 从 IO callable 中提取。
- 活动继续负责 reading/cooking/publishing/retry/close。
- 打包和项目发布使用原模块，不复制新的目录 owner。

若本轮没有第二类导入需求，通用 ImporterRegistry 不是必要交付。局部 Read/Cook/Encode 函数对象是合理的 Process 工作载体，不因为“对象有 operator()”就判为职责错误。

## 10. 需要加强类型，而不是再拆执行者

### 10.1 ProjectCatalogSnapshot（EC1 已知）

private owner 配 public 可写的 version/name/assets，可能破坏观察与保活的一致性。改成 private immutable Data、只读访问器，并在同一 Data 上提供索引查询。不能增加一个 SnapshotReader 来掩盖原字段仍可任意修改。

### 10.2 CompiledMaterial / CompiledFlow（本次加深）

两者同时表达 source、artifact、bytes 和 provenance。[R14,R15] 它们是多份相关表示，不是四个任意可组合字段。

当前通常经 shared_ptr<const ...> 发布，但公开 aggregate 仍允许外部先建立不一致对象，再转成 const。建议由编译／合法构造边界形成 sealed result；只在实际消费需要时重验外部来源，不让所有内部层不断检查四个 nullable 字段。

这不是将源、artifact、编码字节强行合成一份物理存储。它们可以是不同表示，但关联必须清楚。

### 10.3 ResultIntent / WorkspaceIntent（EC1 已知、本次追到消费者）

ResultIntent 的 action 与独立 target variant 可以错配；`receiveResultIntent()` 再按 action 调 `std::get<T>`。[R08]

建议用动作专有载荷的有限 variant。不能宣称现有 UI 已经构造了错误组合，也不能据此扩大为通用动态命令框架。数个私有小值集中在一个语义头足够。

WorkspaceIntent 同理。UI 发命令时应能看出动作需要的参数，不能靠多个未使用字段的默认值表达。

### 10.4 PreparedSessionData（EC1 已知）

它持有延迟 Prepare/Reload 闭包与代码寿命，属于一次性准备操作，不是普通 decoded Data。目标名称 SessionPreparation 足够；无需新增仅转发的 SessionPreparationExecutor。

### 10.5 ProjectOpenData（本次新增命名判断）

它除了 manifest、mounts、digests，还含不可复制的 ProjectWriteLease。[R17] `ProjectStorage::open()` 实际安装 VFS mount 与读端点后消费它。[R04]

因此更准确的是 `PreparedProjectOpen` 或明确“拥有型打开准备”。保留 lease 与 mount 的完整拥有单元，不因为叫 Data 就改成可复制或让 worker 随意销毁 active Project。

### 10.6 Receipt 与 ongoing state

收据代表已经发生的事实；状态记录代表还可能推进的活动。`LayoutCommitReceipt` 的命名问题属于这一类。类似地，ArtifactPresentation 不应靠名称将 ongoing task 声明为纯显示数据。

---

## 11. 明确不建议继续拆的类型

### 11.1 SessionState / EditGate

`SessionState` 组合 binding、binding revision、observed、checkpoint 和 gate，提供 accept/rebind/prepareClose；`EditGate` 已经单独承担同步准入作用域。[R21]

这些是同一工作副本的一致性约束。没有证据需要拆成 BindingManager、CheckpointManager、PermitManager，再由外部协调四套独立状态。

它不是只有 public fields 的状态快照，但这可以用契约说明解决，不必为了名字强制拆。History 的数据／执行分离是特定语义决定，不是所有 State 类的一般法则。

### 11.2 SceneSession / MaterialSession / FlowSession

工作副本 owner 提供 apply/undo/capture 是合理聚合入口。内部可委托原算法或 EC1 的 EditExecutor；不应建立逐方法转发的 SceneSessionController。

### 11.3 SceneProjection / ScenePresentationHub

当前 SceneProjection 公开 instance/version/update，并持有实际运行投影的寿命；Hub 复用且限制记录。[R16] 它不是 PreparedGraph 式纯产物。

已有 `buildSceneSnapshotPackage` 和 `instantiateAuthorProjection` 已表达纯包组装与运行实例化边界。可以优化变化处理，但不能为了“Projection 是名词”复制三份投影状态或建立第二 Runtime。

### 11.4 MaterialCompileOperation / FlowCompileOperation

它们拥有真实任务控制和结果，禁止复制有实际理由。[R14,R15] 仍应有 cancel/ready/result。编译结果与控制对象本来已经分开，所需整改是 target/provenance 和 preview 消费接口，而不是删掉 Operation。

### 11.5 ProjectPublicationOperation / WriteCoordinator

前者实际驱动跨文件与项目采用；后者维护发布顺序和票据。[R05] 它们是执行者，不是错误命名的数据。应尽量消除上层复制流程，不能将它们改成 public 状态袋再让 UI 逐字段推进。

### 11.6 SceneDescription / WorldDescription / ModelSource

SceneDescription 仅拥有记录、payload、索引和借用观察，Builder 才能构造内部记录。[R22] ModelSource 与 cookModel 已按处理边界分离。[R20]

它们是良好对照。数据有 find/size/retainedBytes 不等于在执行外部业务。不要为这些 getter 增加执行者。

### 11.7 PreparedViewState / DetachedView / CodeLease

这些虽然名字像值，却表达一次采用许可或资源拥有。apply/close/destructor 行为可以是其契约的一部分。应检查移动、销毁与回调安全，不将其拆成无法一并管理的裸 Pane、裸 code 指针和若干外部回调。

## 12. 与 AGENTS 和原 EC1 的关系

AGENTS 的底层依赖、数据组件不 include 行为、公共头依赖、异常和 observer 规则继续适用。特别注意：

- 本次推荐的 pure draft／plan 不能 include Pane、Root、RenderRuntime 或旧 ApplicationImpl。
- 项目级 sinclude 与模块私有 pinclude 的能力不能被误当公共 SDK；外部扩展应安装真正需要的契约。
- RAII 保活不等于执行许可；代码 lease、任务寿命和业务 gate 各自的边界不能互相代替。
- 保留跨 callback/async/IO 后的重验；仅合并同一有效期内的重复 lookup。
- 复杂条件按用户具名 bool 分组，并保持短路。不能为了“消灭奇怪谓词”反过来违反 AGENTS。
- 不把所有查询改成信号，不把有限任务从 Process 搬到新的 worker 系统。

本报告不建立第二施工账本。采用的事项只增补到原 EC1 的 S0 清单和唯一 `.internal/editor-redesign/` 材料，历史 P00–P13 不改写。

## 13. 建议并入 EC1 的优先顺序

| 顺序 | 目标 | 为什么先做 |
|---|---|---|
| A | ProjectPublication 的 plan／reservation；已知快照与 intent 不变量 | 固定后续公共能力的寿命与输入语义 |
| B | SceneConfiguration 的 pure draft／准备算法 | 是统一场景能力规则和非 UI 编辑的前提 |
| C | 发布流程回到原活动，内置面板改用正式窄能力 | 让内置与插件真正使用同一基础设施 |
| D | MaterialPreview 配方、结果消费和编译键分域 | 支持独立预览和多个编辑器组合，不影响编译控制 owner |
| E | Workspace 迁移与查询分离；模型 recipe codec | 清理政策与 IO 混合，并获得更清楚的性能边界 |

“优先顺序”不是要求无条件增加所有目标类。选定一个拆分前，应把原数据、实际算法、现有 provider 和消费者对应到表；能用自由函数和已有 owner 完成的，不再增加实例。

## 14. 每次职责迁移的最小证明

1. **算法确实迁走**：旧方法不再保留主逻辑，不能只新增调用壳。
2. **只有一个 owner**：票据、实例、source、checkpoint 和阶段不得复制。
3. **输入不可偷换**：固定来源、target、catalog revision、manifest version 均保持。
4. **失败不丢事实**：文件已发布与目录采用失败分别报告；BUSY 保留；终结错误可恢复。
5. **调用边界一致**：内置与外部安装消费者使用相同 API，不 include AppImpl。
6. **不新增无意义成本**：同一不可变数据不为类型拆分多拷一次；无状态算法不要求每次堆分配。
7. **销毁顺序正确**：owner/lease/worker/code/Pane 谁先结束，在移动赋值、取消和失败路径同样成立。
8. **保留原功能语义**：不以类型名或测试数量替代行为映射，不将旧免验记录改成 PASS。

对本次新发现，先建立针对性测试。无需重跑已经免验的旧人工流程、补满旧慢算法样本或建设 Linux 环境；也不能拿旧免验替代新 API 的实际验证。

## 15. 最终评价

当前问题不是整个工程都没有分层，也不是所有名词都不能有动词。真实问题集中在几处：

- prepared data 隐含活动权限；
- UI 树是业务草稿的唯一载体；
- Application 既组合，也亲自实现可复用业务流程；
- 内置面板依赖私有环境，而外部插件只能使用窄接口；
- 编译结果、投递目标、预览采用和错误域没有完全分清。

收敛之后应出现的是更直接的关系：

```text
领域数据由领域构造／验证
执行者处理明确输入并记录结果
资源 owner 保持完整生命周期
控件只编辑草稿与展示观察
Application 只组合必须跨层的用户流程
```

History 只是这一准则的一项应用。最有价值的下一步，是沿本报告中的真实责任切口迁出代码，而不是继续按名字批量制造 Data、Executor、Manager。

---

## 附录：来源索引

以下均为固定审阅提交的仓库路径；读取范围与链接在 `SOURCE_INDEX.json`。范围为关键区段的文件不意味着已经完整审核整文件。

- **R01** `editor/workbench/scene/src/SceneConfigurationElement.cpp` — SystemElement / ConfigurationField / systemOptions / build / load
- **R02** `editor/workbench/scene/src/SceneCreationView.cpp` — SceneCreationView::Impl：表单、请求与消费
- **R03** `editor/activities/project/include/lux/engine/editor/storage/ProjectPublication.hpp` — ProjectPublication / ProjectPublicationReceipt / ProjectWriteLease / ProjectUpdate
- **R04** `editor/activities/project/src/ProjectStorage.cpp` — ProjectPublication 析构／移动；preparePublication／adoptPublication；目录与读取生命周期
- **R05** `editor/activities/project/src/ProjectPublicationOperation.cpp` — 已有 FILES/PACKAGES/MANIFEST/ADOPT 执行过程与 worker 捕获
- **R06** `editor/application/src/EditorArtifacts.cpp` — receiveArtifact / settleArtifacts：打包、文件发布、目录采用
- **R07** `editor/application/src/EditorWorkspace.cpp` — 读取返回内容截至 WorkspacePane 与注册；末端输出截断，不宣称完整读完
- **R08** `editor/application/src/EditorResults.cpp` — receiveResultIntent / ResultsPane：App 私有状态和多个服务查询
- **R09** `editor/activities/workspace/include/lux/engine/editor/workspace/WorkspaceStore.hpp` — WorkspaceStore / LayoutCommitReceipt / LayoutChoice / LegacyMigration
- **R10** `editor/activities/workspace/src/WorkspaceStore.cpp` — read/listLayouts/chooseLayout/layoutResult 与写入
- **R11** `editor/activities/workspace/src/LegacyWorkspaceImporter.cpp` — geometry / prepareLegacyMigration / continueMigration
- **R12** `editor/activities/material/include/lux/engine/editor/material/MaterialPreviewStore.hpp` — 单目标预览公开契约
- **R13** `editor/activities/material/src/MaterialPreviewStore.cpp` — sphereImage / previewWorld / createPreview / receive / update / reset / navigate / close
- **R14** `editor/activities/material/include/lux/engine/editor/material/MaterialCompilation.hpp` — MaterialCompileKey / CompiledMaterial / MaterialCompileOperation / 错误域
- **R15** `editor/activities/flow/include/lux/engine/editor/flowforge/FlowCompilationService.hpp` — CompiledFlow / FlowCompileEnvironment / FlowCompilationService / FlowCompileOperation
- **R16** `editor/activities/scene/include/lux/engine/editor/scene/SceneProjection.hpp` — SceneProjection / ScenePresentationHub / ProjectionEnvironment
- **R17** `editor/activities/project/include/lux/engine/editor/storage/ProjectOpenData.hpp` — ProjectOpenData 含 ProjectWriteLease
- **R18** `editor/activities/project/include/lux/engine/editor/assets/AssetImporter.hpp` — 模型导入的请求、结果、生命周期
- **R19** `editor/activities/project/src/AssetImporter.cpp` — Load/Read/Cook/Encode 与 model-source recipe
- **R20** `engine/toolchain/asset/model/include/lux/engine/toolchain/asset/model/ModelCooker.hpp` — ModelSource / ModelCookProduct / cookModel / readModelSourceFiles
- **R21** `editor/editing/include/lux/engine/editor/sessions/SessionState.hpp` — EditGate / SessionState / checkpoint / permits
- **R22** `engine/scene/description/include/lux/engine/scene/SceneDescription.hpp` — 纯描述、借用视图、builder 与私有存储
- **R23** `modules/function/render/README.md` — 渲染的领域边界；只作为设计说明，不当运行证明
