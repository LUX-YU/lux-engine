# P11 — 正式命令、不可变贡献与两阶段工厂

本文件可独立交给实施 LLM，但必须同时遵守 `00_MASTER.md`。只执行 P11，完成后停下复审，不自动执行 P12。

参考：`lux-engine@22ab1a30862f6bff943cef86d4232839173c7107`。本文件中的“新增”是目标签名/职责，不声称当前仓库已有实现。

## 1. 完成定义

P11 完成时，必须同时成立：

1. 正式新工作台的菜单/快捷键使用分离的 query 和 execute；点击目标和注册绑定政策明确。
2. query、execute、创建、恢复和清理期间，当前 entry、闭包、元信息和代码 owner 不会被注册更新提前释放。
3. Scene/Material/Flow 都能经正式工厂读入、在 owner 构造并完整发布会话与角色，不由 worker 构造线程亲和对象。
4. 正式视图工厂只产生完整 DetachedView，不接管、聚焦或借旧 PaneManager 打开资产。
5. 至少一个真实外部 Editor 扩展仅靠安装 SDK 构建、装载和使用；不兼容版本在贡献回调前拒绝。
6. 已迁出的原算法/注册定义只有一份。仍服务旧 executable 的有限声明有明确消费者和 P12 删除项，不建立新兼容桥。
7. 五层边界、C03 契约和既有保存/运行/交互回归仍成立。

本阶段不是“给每个功能添加 virtual 接口”。新增类型有真实消费者才加入；一个语义文件可以容纳多个小型值。

## 2. 当前输入与第一批检查

### 2.1 必读现有位置

| 文件/目录 | 用途 |
|---|---|
| `editor/metadata/.../CommandRegistration.hpp` | 当前混合 `EditorContext& + ui::Command` 的 QUERY/EXECUTE 回调。 |
| `editor/context/.../EditorContext.hpp` 与 CPP | setCommands、commands span、assetEditors、PaneManager 等旧消费者。 |
| `editor/metadata/.../EditorPluginExports.hpp`、`src/EditorPlugin.cpp` | 当前 V6 出口及原插件验证/装载，不重新发明 loader。 |
| `editor/metadata/.../ComponentEditorRegistry.hpp`、`ConfigurationValue.hpp`、`SceneRegistrations.hpp` | 分清纯值、UI 创建、元信息和应用安装。 |
| `editor/editing/include/.../SessionStore.hpp` | 预留、准备、发布与 owner 限制。 |
| `editor/activities/persistence/include/.../SaveService.hpp`、`SaveSource.hpp` | 现有保存角色及可靠完成纪律。 |
| `editor/workbench/desktop` | Host/Shell、DetachedView、关闭错误、Root 安全点。 |
| `editor/app/product/ProductAssembly.cpp`、`ProductCommands.cpp` | 旧内置贡献实际清单，不能只注册新测试命令。 |
| `editor/app/test/baseline_failures.cpp` | C03 原可观察契约，以及不得改义的 C01/C04。 |
| `dev_log/P10Q-structure/retained-product.json`、`target-map.json` | 当前 provider/consumer 的起始事实，不直接作为未来最终清单。 |

若路径已变，沿 Git 和 file-plan 定位唯一新位置，不恢复已删除目录。不要只看 README 就宣布协议已经迁移。

### 2.2 冻结四张映射

在现有账本记录：

- 每个内置命令：旧 id、菜单位置、快捷键、query 输入、execute 目标、调用的真实业务、接收者寿命。
- 每项贡献：稳定类型/SchemaId、反射来源、旧 factory、所有消费者、代码 owner、目标层。
- 每种会话：load/decode/construct/publish/close、历史/保存/SaveAs 能力和具体实现。
- 每项 SDK 出口：公共头、导出函数/版本、库、运行依赖、消费者和最终删除时间。

旧主菜单的用户功能必须全部映射。P12 才实现的产品组合命令可暂不在新正式菜单显示；不得以“永远返回 Unsupported 的处理器”占位并计入完成。

## 3. 最小类型与文件布局

### 3.1 命令

建议目标（同模块内，不是一行一个库）：

```text
editor/activities/commands/
  include/lux/engine/editor/commands/Command.hpp
  include/lux/engine/editor/commands/CommandRegistry.hpp
  src/CommandRegistry.cpp
  src/CommandDispatcher.cpp
  test/commands.cpp
```

| 类型组 | 责任/所有权 | 禁止承担 |
|---|---|---|
| `CommandDescriptor` | id、显示名、分组、默认键位的纯描述 | EditorContext、Pane*、可变注册容器 |
| `CommandQuery` / `CommandState` | 一次只读查询的确定目标与可用/勾选/原因 | 遍历全局服务、保存完整作者源、隐式切换目标 |
| `CommandInvocation` | 触发时固定的身份、明确参数、来源策略 | 延后执行时读取当前焦点、裸临时 span |
| `DispatchReceipt` | 区分立即结果、已接受业务操作、拒绝 | 将 Task.finished 冒充业务完成 |
| `CommandEntry` | query/execute 的实际 callable，外层代码 owner | 同一语义再包 Port→Provider→Adapter |
| `CommandRegistrySnapshot` | 不可变条目集合和修订 | 回调内原地 erase/替换 |
| `CommandHandle` | 固定 snapshot/entry，调用期持有寿命 | 借一个容易失效的 span 或只保存下标到 current vector |
| `CommandDispatcher` | 有界等待、检查目标/注册政策、执行 | 第二事件总线、通用工作流系统、领域编辑权威 |

Descriptor 的显示信息与快捷键映射可在同一值中，但 activities 不得 include 带 Pane/ImGui 的 Menu 实现头。复用已有纯身份；若当前 CommandId 与重 UI 头绑定，只提取唯一纯声明，不另造两套相同字符串 ID 再转换。

### 3.2 工厂与角色

```text
editor/activities/sessions/
  include/lux/engine/editor/sessions/SessionFactory.hpp
  include/lux/engine/editor/sessions/SessionInstallation.hpp
  include/lux/engine/editor/sessions/SessionActions.hpp
  src/SessionInstallation.cpp
  test/installation.cpp
```

这些逻辑 include 属于各自真实 provider，不得让 `editing` 因共享名字前缀而链接 activities。

| 类型组 | 决定 |
|---|---|
| `SessionKindDescriptor`、`SessionFactoryRegistration` | 描述支持的源与实际 load/owner-construct 入口，放 activities；不把 UI 创建混进来。 |
| `SessionLoadJob` / `PreparedSessionData` | 复用现有具体 codec 与 Process；拥有 decode 数据、代码和环境，不能持 Pane。 |
| `PreparedSessionInstallation` | move-only 的未发布安装责任：原 reservation、角色 owner、已预留的注册位置；不第二次拥有已交给 Store 的 Session。 |
| `InstalledSession` | 会话 ID 对应的角色/注册 token/关闭依赖元数据；Session 本体仍只在 SessionStore。 |
| `HistoryActions` | 调用真实具体历史的窄能力；不再创建 UndoStack。 |
| 保存/SaveAs 能力 | 优先直接复用 ISaveSource/FrozenSave/IPreparedRebind。若描述可用性已经足够，不额外制造一套 SaveAsActions。 |

具体 Scene/Material/Flow 工厂实现在现有 `activities/{scene,material,flow}/src` 中，可各一个 `SessionFactory.cpp`。不要重建旧 `tools/*/integration` 目录。静态 helper 实例化在这些提供者 CPP 中，通用 sessions 头不 include 三个具体模型。

### 3.3 工作台工厂与应用安装

```text
editor/workbench/desktop/
  include/lux/engine/editor/views/ViewFactory.hpp
  src/ViewFactoryRegistry.cpp
  src/CommandMenu.cpp             # 原 UI 机制与正式命令的接线

editor/application/extensions/
  include/lux/engine/editor/extensions/EditorExtensionExports.hpp
  src/LoadEditorExtension.cpp
  src/InstallEditorContributions.cpp
  src/BuiltinContributions.cpp    # 内容过多才按真正领域分 CPP
```

`EditorExtensionExports.hpp` 是外部模块加入整个 Editor 时的装配契约，可以组合下层贡献类型；E0–E3 不能反向 include 这个总出口。下层各自发布窄契约，不建立人人都要 include 的 ExtensionHost。

通用视图工厂注册只保存创建/恢复纯 UI 配置的能力；创建参数携身份与明确初始绑定。不得把 `ViewHost&`/`PaneManager&` 或可定位任何服务的 Context 传入 factory。

## 4. 命令的目标和执行规则

### 4.1 四种输入关系不要压成一个布尔开关

| 命令类型 | 固定内容 | 执行时核验 |
|---|---|---|
| 保存 | 原 SessionId、项目/绑定语义 | 在实际 admission 捕获该会话当前已提交内容；不因焦点变化转向别的会话。 |
| 删除选择 | 原 Session/History、用户当时的具体对象集合 | 对象仍属于该域且存在；严格编辑按需要保留完整 expected。 |
| 严格草稿提交 | 原 payload + based_on | 内容变化明确冲突；不得执行时补 current。 |
| 仅显示/聚焦 | 原 ViewId/代际 | 目标仍是同一视图；失效拒绝，不查同类型第一个替代。 |
| 应用级命令 | 明确 application 作用域 | 执行阶段与授权能力，不依赖当下任意 Pane。 |

注册绑定同样显式：默认 `PINNED`，队列持接受它的 entry；少数明确选择 `CURRENT_REGISTRATION` 的命令在执行时重新查同 id，重新验证输入兼容性和目标政策。禁止 revision=0 这种魔法哨兵。

### 4.2 参数与错误

不能要求所有领域 edits 都经过命令总线。生成 Inspector/Graph 手势继续直接使用具体领域接口。

参数封闭时用具体值/variant；外部开放参数需要类型擦除时只擦除一次，payload 必须拥有数据与代码寿命。可由注册的 prepare-invocation 函数构建绑定具体参数的 owning callable；不建立 `map<string,any>` 大袋子，也不让 UI 保存借用 payload。

`DispatchReceipt` 只表示派发事实。AcceptedOperation 可以携现有 SaveId/CompileId/RunId 的标识或窄观察描述，不再存一份它们的终态。不存在的通用 cancel 不要为了接口对称而增加。

返回原领域失败或一次准确诊断投影；BUSY、STALE、CLOSED、权限、版本冲突、Unknown 不能都映射为“不可用”。

### 4.3 查询不能隐藏写能力

查询输入应由装配提供必要只读状态，而不是整个 Context。查询不打开资产、不启动任务、不修改焦点、不提交注册。

C++ const 不是插件安全沙箱。测试仍要覆盖 query 企图更新注册、销毁自己持有的句柄、递归派发等情况；更新只能进入 pending 或准确拒绝。不能声称任意恶意回调已被隔离。

## 5. 不可变注册的具体实现纪律

### 5.1 条目只构造一次，快照共享条目

推荐结构：snapshot 拥有只读 entry 列表；entry 外层持 CodeLease，再持 owning callable/元信息。

有 move-only callable 时，不能为了发布下一版本重新复制全部闭包。新 snapshot 可以共享未变化的 `shared_ptr<const CommandEntry>`，只构造变更条目。不要用不可复制闭包逼迫整个注册体系改回 std::function 或 raw void*。

snapshot/entry 是有明确 lifetime 的共享，不是所有操作都变成 shared ownership。可调用控制 owner 的特殊成员须显式裁定。

### 5.2 调用期必须独立持有

query/execute 进入时先取得一个本地强 handle/entry 及代码 owner；回调返回和错误值构造结束前不释放。只保证 registry 对象活着，不能保证原条目活着。

不允许外层保存 `Entry&` 后执行回调，再访问那个可能被回调撤销的可变容器。调用 helper 可以按值接收 handle，避免原持有者在回调中释放自己后破坏正在用的记录。

### 5.3 发布点

1. 验证完整候选：重复 ID、缺 callable、非法键位/元信息、容量及代码 owner。
2. 失败保持原 snapshot；候选清理在自己的 code 生命周期内。
3. owner 安全点发布；不得在 query/execute/工厂/通知/析构回调栈中直接替换。
4. old snapshot 由在途 handle 自然释放；待发布更新有界、策略明确，不能无界积累。
5. 通知失败不撤回已经发生的发布；保留 revision，消费者可恢复同步。

整批布局/恢复固定同一组 ViewFactory/命令/内容工厂快照。application 可以为一个批次组合几个只读 handle；E2 不因此知道 E3 类型。多注册目录安装时先全准备，在一个不可重入、无任意回调的发布点变为可见；不把“逐个替换并触发通知”称为原子安装。

## 6. 两阶段内容工厂：发布前不能有半个会话

### 6.1 阶段图

```text
接纳打开请求与固定工厂
  → worker：读取拥有型 bytes / decode / 校验预算 / 返回 PreparedSessionData
  → owner：reserve<T> 取得真实 SessionId
  → owner：构造具体 Session、History、角色
  → owner：SessionStore.prepare 转移唯一 Session 到不可见槽位
  → owner：完成角色注册位置、关闭 hook 等准备
  → 一个安全发布点：Session 与角色共同可用
  → 发布后通知
```

禁止 worker 创建 EditHistory、SessionState 或 LuxObject 后再搬到主线程。PrepareData 不得捕获临时 Registry、TaskReporter 引用、Pane、未保活的元信息 span。

`SessionStore.prepare()` 之后，Session 已在 Store 的未发布槽位，不能继续让候选 unique_ptr 保有它。reservation 失败清理必须撤销该隐藏槽位；清理角色后才能结束它所借用的 Session/代码。

### 6.2 现有接口不足时怎样补

当前 `SaveService::registerSource()` 是立即注册入口，不能直接假定它能与另一个 owner 的 publish 原子组合。

先证明现有 gate/注册可见性是否足够。如果不够，只添加保存注册的窄 prepare/commit/abandon 机制，与 `PreparedSessionInstallation` 组合：

- prepare 预留资源但外部查询不可见；
- commit 只进行事先准备好的无普通失败状态转移，不执行外部 callback/分配/IO；
- abandon 释放准备资源，不调用不存在会话的角色；
- 原 registerSource 可由这套唯一算法实现，但不能保留第二套注册算法。

E0 SessionStore 不 include SaveService。跨 owner 的安装编排在 E2；E0 只提供会话发布的必要事实。不得建立公共 TransactionManager 为这一项兜底。

### 6.3 必须插入失败点的位置

读入前；decode 后；reserve 后；Session 构造后；Store.prepare 后；第一角色准备后；最后角色准备后；publish 前；通知时。

在每个 publish 前失败点都检查：Store 已发布数量不增加、无可查新 SessionId、无对外可调用的保存/历史角色、输入/代码只释放一次、后续正常安装仍可用。

publish 后通知失败要报告“已安装但通知失败”，不能抹掉会话或伪装未发生。

## 7. 代码 owner 与 ABI

### 7.1 每一种晚释放对象都要列入

命令 callable、prepared data、动态 Session、SaveSource、编码 job、rebind candidate、DetachedView、反射/配置值、错误 payload、已返回的编译/运行结果。

外层 code 字段先声明只是必要条件之一。还要检查移动赋值、swap、reset、异常展开和虚析构尾部；旧 payload/对象析构时旧 code 必须仍然存在。

不要让插件对象仅以自己的成员 CodeLease 自证安全；该成员结束后虚析构尾部仍可能在插件代码中。使用原 Store/DetachedView/OwnedEncodeJob 等外部拥有关系。

### 7.2 当前 V6 出口与新出口

当前已核对 `lux_editor_exports_v6` / interface_version=6，且表里包含旧 PaneRegistration、AssetEditorRegistration、CommandRegistration。

P11 定义新的 Editor 贡献契约版本和出口（具体新编号先核对当前分支，不能复用相同版本改变布局），先验证 header/version/大小/数量/依赖，才调用贡献入口。旧 V6 仅作为负例输入，不由正式新 loader 静默接受。

复用 `lux::project::loadPluginLibrary`、PluginDescription、runtimeCode 和依赖 pin。不要在 Editor 再实现 LoadLibrary/dlopen 封装或第二 PluginManager。

SDK 指纹应限定真正的 C++ 接口和工具链契约，不把 editor 的变化污染 runtime 插件；也不宣称哈希相同就意味着跨所有编译器 ABI 稳定。

### 7.3 首版不做任意热卸载

有对象/任务/回调在途时，卸载请求准确拒绝或等待原 owner 退休。只改注册快照不等于 DLL 已卸载；最后资源、payload、动态 deleter 结束后才允许释放原 code owner。

缺少真实装载运行的测试，只能称编译/链接资格；不能以 shared_ptr witness 代替全部插件装载资格。

## 8. concept / virtual / signal 的落地选择

| 场合 | 选择 |
|---|---|
| 具体工厂和 codec 的编译期适配 | concept + 小 helper，实例化在提供者 CPP。 |
| 未知 Session/角色/视图 factory 的运行时集合 | 复用窄 virtual 或单次 owning type erasure。 |
| 通知目录/命令可用性变化、控件事件 | 原 LuxObject/Connection。 |
| 返回 Result 的同步查询/准入 | 普通准确方法，不强行改成无返回信号。 |
| 某种业务没有替换需要 | 具体服务引用，不为 mock 创建一套 I 类。 |

可使用如下目标签名检查；实际 Result/类型名以唯一生产定义为准，不复制示例类型：

```cpp
template<class Q>
concept CommandQueryCallable = requires(Q& query, const CommandQuery& input) {
    { query(input) } -> std::same_as<CommandResult<CommandState>>;
};

template<class E>
concept CommandExecuteCallable = requires(E& execute, CommandInvocation& input) {
    { execute(input) } -> std::same_as<CommandResult<DispatchReceipt>>;
};
```

执行器仍拥有排队 invocation。execute 的可修改引用仅用于成功准入后的拥有型输入交接，目标、注册政策和 based_on 不得被重写；BUSY 必须保留原载荷。若任务 API 在准入成功后才调用 factory，应在 factory 内移动输入，不能提前按值消费后再声称可重试。

不要给能够分配/解码的函数无依据强加 noexcept。concept 证明不了 query 无副作用、snapshot 深层拥有或工厂线程亲和；这些另用真实测试证明。

不要求引入所有列出的概念。只有存在正式 helper 和真实用途才添加；至少用两种实际内置类型或一个真实开放扩展验证它。独立玩具示例不是 SDK 正例。

## 9. 各批具体施工与删除

### P11-A：登记真实消费者

读取当前 frozen 账本，展开 metadata/plugins/context/app 的实际 source、public headers、生成输入、target、安装包和依赖。

输出本阶段精确成员计划，不复制一整份新的可变账本。所有 `consumers=[]` 的生成目标先查 generator 和 MANUALLY_ADDED_DEPENDENCIES，不能直接删。

### P11-B：命令核心与菜单接点

先实现 B 节的数据、目标政策和 snapshot；再在新工作台使用真实菜单和快捷键产生 invocation。替换新链的旧 `ui::Command::QUERY/EXECUTE` 混合入口。

普通点击不能新建用户看不见的会话。queued target 失效应拒绝，不转向另一个同类型窗口。实际删除目标和替换 registry 的测试必须在入队与执行之间发生。

### P11-C：内容工厂与安装

实现三类真实 decode/construct/roles 发布；复用对应 `SceneCodec/MaterialCodec/FlowCodec`、PersistenceAccess 和原模型创建器。

没有定义支持的源类型不得假成功。无保存能力/无历史能力的类型应有准确 capability，不能将所有工具强制变成作者 Session。

### P11-D：贡献拆分

- ConfigurationValue 的纯值/codec/反射事实归适合的作者域或已有正式值。
- 组件/配置 UI factory 归 workbench 对应域，复用原生成器，不复制生成实现。
- 保存/内容工厂归 activities；UI factory 归 workbench。
- 装载和组合归 application/extensions。
- 原 engine runtime 插件接口和纯 schema 不跟 Editor 总表一起改名。

### P11-E：真实内置与外部消费

Scene/Material/Flow 的内置注册由 application 组合，业务 consumer 只接收其必要服务。

至少验证一个外部安装扩展：一个实际可构造/销毁的最小 Session 或对现有 Session 的保存角色，一个 DetachedView 和一个 query/execute 命令；验证结果、关闭和最后代码寿命。无实际贡献的空 DLL 不计。

### P11-F：删除与旧产品短暂共存

已经迁出的配置/控件/反射算法删除原体，更新所有直接消费者。优先使旧产品直接消费同一正式不可变注册能力；不能把旧 `invoke(Context&, Command&)` 包在一个同名新接口里冒充迁移。

旧 product 在 P12 才切换，无法独立替换的原工厂声明/创建接线可按 D04 逐项暂留，必须给出路径、当前消费者和 P12 的具体替代步骤。禁止增加新的 bridge、扩展旧白名单或保留零消费者文件。

C03：正式新路径的同义原断言必须通过。若旧 executable 当前仍保留 query 路径，其活动 probe 也不得继续发生代码提前释放；可用调用期保活的最小修正或直接迁移到同一已验证机制，不重写第二命令框架。修正记录随旧实现 P12 删除。历史 C03 FAIL 证据原字节保留。

本阶段结束不能宣称旧工厂所有头已删除而实际仍导出它们。交接中分列“P11 正式新契约”“P12 旧产品最后消费者”；P12 前冻结的有限剩余项不得再次增长。

### P11-G：资格与停点

同一最终实现 SHA：完整适用 Windows 构建、二次无工作、原回归、真实依赖负例、安装 SDK、新外部扩展、C03、新工作台命令场景。

C01/C04 若仍是旧路径失败继续准确记录；不得把它们泛化为任何未修问题的豁免。输出实现与独立验收提交，停在 P11。

## 10. P11 验收主题

沿用原 X11-01–07 的含义；下面的补充属于这些主题，不要求每行一个 executable。

| ID | 必测输入/时机 | 观察 |
|---|---|---|
| X11-01 | query 请求替换自身；释放外部 handle；闭包继续执行 | 原 entry/代码到返回后仍活；更新延迟；无悬垂或重复调用。 |
| X11-02 | 一批视图/内容准备中，第一个 factory 请求更新贡献 | 全批固定原 snapshot；失败候选清理代码顺序正确；下一批才见新版本。 |
| X11-03 | 点击 A→入队→焦点 B→替换注册→A 被关/槽位复用 | PINNED 正确、CURRENT 明确；不漂移；旧身份拒绝；参数非借用。 |
| X11-04 | worker decode、owner 延后构造，逐角色注入准备失败 | Session/History owner 线程正确；任何提前失败无半发布、无半注册。 |
| X11-05 | 命令/动态源/View/错误值最后 owner 与虚析构尾部 | 最后外部 code pin 晚于所有使用；move/reset/异常展开也正确。 |
| X11-06 | 新安装前缀构建真实外部扩展，另装不兼容 V6 | 正式扩展可用；V6 拒绝在贡献调用前；无源树私有 include。 |
| X11-07 | 更改 Editor 契约版本，独立构建 runtime 插件 | runtime 不引入 Editor，拒绝/升级范围准确。 |
| X11-A | 注册队列满、查询递归、调用中取消/销毁 | 无无限排队，无角色过早销毁；BUSY 不丢已接受任务完成。 |
| X11-B | Save 在旧菜单点击后源更新；DeleteSelection 的原集合改变 | 各自政策正确，不把所有命令用同一 revision 规则。 |
| X11-C | factory 不匹配、重复类型、未知配置、预算失败 | 完整批次拒绝或按明确策略跳过；无默认成功空实现。 |
| X11-D | 安装后新 Window/Task/Save 分别释放，再释放扩展 | 当前 producer 先撤销，已接受结果能结清，用户数据不被自动删除。 |

负例须失败于指定规则；缺包/缺链接器不算 concept 或禁边成功。Linux/IME 未测不阻塞此阶段限定范围。

## 11. 给 P12 的最小交接

- 实现与验收 SHA、当前真实分支和新依赖/SDK版本；
- 命令与工厂完整入口表；原功能矩阵和尚属 P12 的组合功能；
- 新贡献版本/导出符号与外部 SDK 资格；
- 三类 InstalledSession 所有权、关闭/保存能力和销毁顺序；
- 固定的旧文件/符号/target/安装包/P12消费者清单；
- C03 新旧活动路径结果、历史失败保全、C01/C04当前状态；
- 结构门禁和剩余测试环境范围。

P11 本身不得以“P12 会接线”为理由，只交付没有真实使用者的接口。也不得为让旧产品继续工作而允许任何正式内层依赖旧 Context。
