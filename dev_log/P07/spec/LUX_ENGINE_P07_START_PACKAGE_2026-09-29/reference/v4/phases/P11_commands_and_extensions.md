# P11 — 命令事实、不可变扩展发布与工具工厂

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P01, P05, P08, P09, P10。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M13 / M14 / 三类工具的 integration。**关联问题：** A03、A04、A05、A07、C03、B04。**V3 回归：** Q38, Q39, Q40, Q41, Q46, Q48, Q52。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `CommandDescriptor / CommandState / CommandQuery / CommandInvocation` | `editor/commands/include/lux/engine/editor/commands/Command.hpp` | 描述、只读输入、确定目标意图 | query 不取全局 Context；execution 重新验证目标和可用性。 |
| `DispatchReceipt / OperationRef / RegistryBinding` | `editor/commands/include/lux/engine/editor/commands/DispatchReceipt.hpp` | 准入结果/仅 UI 跟踪身份/明确注册绑定策略 | ImmediateCompletion 与 AcceptedOperation 分开；不携所有业务状态的全局 any。 |
| `CommandEntry / CommandRegistrySnapshot / CommandHandle` | `editor/commands/include/lux/engine/editor/commands/CommandRegistry.hpp` | 不可变记录和拥有 snapshot 的调用句柄 | query/execute 同样保活 entry+code；批次内不替换正在迭代容器。 |
| `SessionKindDescriptor / ViewFactoryRegistration / SessionFactoryRegistration` | `editor/extensions/api/include/lux/engine/editor/extensions/Registrations.hpp` | typed 描述与执行入口组合 | 构造、接管、复用、显示分开；无 PaneManager& factory。 |
| `SessionLoadJob / PreparedSessionData / PreparedSessionInstallation` | `editor/extensions/api/include/lux/engine/editor/extensions/SessionFactory.hpp` | worker 准备角色与 owner 安装值 | worker 只拥有 decode 结果；owner 构造线程亲和 Session/History；发布前角色齐备。 |
| `HistoryActions / SessionRoleBundle / SaveAsActions` | `editor/extensions/api/include/lux/engine/editor/extensions/SessionRoles.hpp` | 按实际需要定义的窄角色适配 | history query/undo/redo、保存源、SaveAs；可缺省角色明确可用性，不用万能服务表。 |
| `ExtensionSnapshot / ExtensionPublisher` | `editor/extensions/host/include/lux/engine/editor/extensions/ExtensionHost.hpp` | 批次发布服务，不是全局 ServiceLocator | 只在 owner 发布阶段替换；在途实例/任务独立保活旧代码。 |
| `SceneToolModule / MaterialToolModule / FlowToolModule` | `editor/tools/{scene,material,flowforge}/integration/` | 装配函数/对象，不给业务传递 | 绑定具体工厂、角色、命令、编译/运行与视图；不封装可达整个引擎的 getters。 |

## C. 命令与领域编辑不是一回事

CommandDescriptor 只包含菜单/快捷键/展示元数据；CommandEntry 组合 query、execute 与外层 CodeLease。SceneEditBatch/MaterialEditBatch/FlowEditBatch 是领域编辑，不经一个全局字符串 CommandBus 才能调用。

CommandQuery 是已经解析的只读目标和必要状态快照；CommandInvocation 固定用户触发时的 ViewId/SessionId 及具体策略。菜单被点击后焦点改变不能让请求作用于另一个内容；目标删除后旧身份失效，不能使用新焦点回退执行。

Save 在执行 admission 时捕获原目标会话的当前已提交状态；DeleteSelection 固定用户当时选中的对象集合再验证；需要严格基线的操作携 ContentStamp。不能用一个所有命令通用的 revision==0 哨兵或“全部版本必须等于点击时”规则。

### C1. 注册生命周期实现

调用方持 CommandHandle，它拥有当前不可变 CommandRegistrySnapshot 与条目索引/引用。query 和 execute 都按这一方式调用，不能只在 execute 复制 callable；恢复布局同样固定 ExtensionSnapshot 到整批准备结束。

注册更新进入 pending publication，owner 批次结束后构造并发布新 snapshot；当前回调继续使用旧条目。普通排队命令默认 pinned 接受它的注册；确实需要执行最新实现的命令用明确 CurrentRegistration 策略，不能由 registry revision 的魔法零值决定。

const query 参数不是同进程安全沙箱。装配不向 query 注入写能力，发布门拒绝/延迟即时替换；测试误用式回调仍必须存在。闭包可捕获外部可写对象的现实不能用 `const` 掩盖。

## D. 工厂的两阶段输入输出

`SessionLoadJob` 在 worker 执行受限读入/decode，产物 PreparedSessionData 拥有数据与所需 CodeLease。其 owner 阶段构造函数接受预留 SessionId，建立具体 Session 和角色 bundle；不在 worker 创建捕获 owner-thread 的 EditHistory 再搬到主线程。

为保持 P05 codec/persistence 不反向依赖扩展 host，Scene/Material/Flow 的 SessionLoadJob 适配壳放各工具 integration，调用 P05 的具体 decode/PreparedData。这是对 V3 模块表的实施澄清：领域 codec 留 M07，动态工厂角色适配留 M16；不增加 M07→extensions host 的隐藏边。

PreparedSessionInstallation 包含一个唯一 Session owner、外层 CodeLease，以及已准备但尚未公布的 ISaveSource/HistoryActions/可用 SaveAsActions。SessionRoleBundle 只列本轮实际用到的角色，不提供 map<string,any>/getService<T>。发布失败整体候选销毁，不能已经在 SaveService 注册一个并不存在的会话。

ViewFactory 接收验证后的具体创建/恢复参数和构造时绑定的窄依赖，返回 DetachedView；没有 Host/PaneManager 的 adopt/show 权限。复用与打开内容由 P12 决定。

## E. typed 扩展、元信息和代码寿命

旧 metadata 中纯 schema、场景配置描述、反射元数据归对应领域或 extension 描述；PaneRegistration/CommandRegistration 这样的可执行入口改到 extension_api/commands。不能只整体目录改名而继续把 UI factory PUBLIC 传播给 runtime 插件。

代码 lease 必须由外层拥有记录保活到虚析构返回之后。一个插件对象把 lease 作为自己的成员，可能在成员析构后仍执行插件析构尾部，不能单靠它证明 DLL 可卸载；Store slot、DetachedView、EncodeJob owner、PreparedData owner 的声明/释放顺序要具体测试。

首版不要求任意插件热卸载。卸载请求遇到在途回调/对象/任务应明确拒绝或标记待退休，不能先 dlclose 再等对象自行释放。保留既有 PluginManager 的低层能力，不新增万能 IPlugin。

## F. SDK 与安装验证提前做

运行插件的签名/指纹输入与 editor extension 分开；改变 editor 工厂契约不应无故让运行插件依赖 editor SDK。只承诺当前约定工具链/标准库/编译选项范围内的 C++ 插件契约，不宣称指纹相等就稳定 ABI。

本阶段至少构建一个新外部 editor consumer，只使用 install prefix 的公开契约创建/注册最小工具与命令；不得 include 源码树 sinclude/pinclude 或 TestAccess。正式安装闭包 P13再做全矩阵，但若本阶段只能通过源树私有头工作，不能进入 P12。

老插件 SDK 通过显式版本拒绝或单次编译迁移，不留永久兼容 vtable；旧格式中的类型名字符串可由 LegacyWorkspaceImporter 解释，不需要保留同名旧 C++ 类。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| 新命令 invoke(EditorContext&, ui::Command&) 统一 QUERY/EXECUTE 协议 | 替换为 query/execute 与 DispatchReceipt；不复制旧协议包装 | CommandEntry / pinned snapshot；P11 | 已验证算法、资产兼容读取与行为回归不删 |
| PaneRegistration::CreateResult/create(PaneManager&)/restore(PaneManager&,...) | 新扩展不允许；调用两阶段工厂和 DetachedView | SessionFactoryRegistration / ViewFactoryRegistration；P11 新路径；旧头 P12 | 已验证算法、资产兼容读取与行为回归不删 |
| 旧 setCommands/setRegistrations 返回 span 的可变集合借用 | 新路径改不可变 snapshot；所有批次持有版本 | ExtensionPublisher / CommandRegistrySnapshot；P11 新路径；旧方法 P12 | 已验证算法、资产兼容读取与行为回归不删 |
| metadata 可执行入口混在纯领域描述中的导出关系 | 拆分真实描述和可执行注册；保留必要 schema/生成算法 | 各域描述 + extension_api；P11 新边界；旧 target P12 | 已验证算法、资产兼容读取与行为回归不删 |
| 全局 SDK fingerprint 将运行和 Editor 一起绑定的输入 | 按实际 API 闭包拆分并验证安装消费 | 独立 runtime/editor SDK contract；P11；P13 最终资格 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X11-01 / `query_replaces_own_registration` | query 请求替换自身后继续访问捕获对象 | 旧 callable/code 在返回前存活；发布延迟 |
| X11-02 / `batch_restores_with_fixed_extension_snapshot` | 第一个 provider 请求更新注册 | 整批使用同版本；无 span 失效或半批新版本 |
| X11-03 / `queued_command_keeps_identity_and_registration` | 点击后改焦点、替换 registry、删除/复用目标 | 按固定策略执行原目标/原entry或拒绝，不漂移 |
| X11-04 / `session_factory_worker_vs_owner` | worker decode，延后 owner construct；角色准备失败 | 线程亲和正确；无半注册 Session |
| X11-05 / `lease_covers_virtual_destructor_tail` | 外部插件对象/任务/闭包析构顺序记录 | 代码卸载晚于最后析构返回，不只晚于成员销毁 |
| X11-06 / `external_consumer_uses_installed_public_api` | 清空源码 include 环境构建最小新插件 | 仅 install prefix 可编译；旧 ABI 拒绝发生在调用前 |
| X11-07 / `runtime_sdk_not_tied_to_editor_change` | 改变 editor 接口版本并构建独立 runtime 插件 | Editor 拒绝/更新范围准确；runtime 无不必要重耦合 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P11.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
