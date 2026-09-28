# P03 R1 复审结论与 P04 启动指令

**日期：2026-09-28**  
**项目：LUX-YU/lux-engine**  
**实施分支：`codex/editor-redesign-v4`**  
**本次读取的 HEAD：`6a4ebe953ad227838147f28c11b2e57cb6d3fb35`**  
**R1 实现：`90d74819a358ac722c002fd544b88cb4fbb454a1`**  
**原 P03 验收：`7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb`**

## 0. 放行决定与文档效力

**P03 R1 在本次源码及归档证据复审范围内通过，可以开始 P04；只执行 P04，完成后停下复审，不自动进入 P05，不修改 main。**

这不是要求重做 P03，也不意味着完成整个 Editor 的工程资格认证。下一阶段仍为 V4 的“FlowForge 作者图与第三工具覆盖”。

本文件是原 `phases/P04_flowforge_authoring.md` 的启动补充。原文档规定类型、职责、迁移和 X04 场景；本文件补入实际起点、已经实施的读取/输入清理契约、目录收拢及 Flow 数据语义。不存在另一份可独立变化的施工账本。

附包中的 V4 原件是只读参考，旧路径和旧 manifest 不得覆盖当前仓库：

- 当前唯一可变施工材料继续为 `.internal/editor-redesign/`。
- 当前冻结记录继续放在 `dev_log/<阶段>/`；P04 新记录为 `dev_log/P04/`。
- 原 `editor/history/`、`editor/sessions/` 已收拢，实际位置为 `editor/editing/history/`、`editor/editing/sessions/`。
- 旧文档中的泛称与既有 C++ 拼写有差异时，采用已有 `TSessionAccess<T>` 等实际类型，不再造同义物。

## 1. 复审范围与证据等级

本次实际完成：读取实施分支 HEAD、验收提交父链、R1 实现差异、完整私有重载头、`MaterialSession::create` 的输入清理、新增真实模型测试、归档 CTest、修复前/后部分日志、R1 验收说明及核验脚本；同时读取当前 FlowSource/FlowSourceEnvironment 和旧 FlowForge 实现头，为 P04 交接校准术语。

本次没有独立重跑完整引擎构建、64 项 CTest、七组安装消费者、GPU 或真实 DLL 卸载；没有执行完整 `check_receipt.py` 或逐份复算远端归档哈希。以下运行成绩属于实施方提交的证据，不写成复审方独立运行结果。

| 层次 | 复审所见 | 边界 |
|---|---|---|
| 源码 | 局部 owning input、READING 内消费、create 保持外层 lease | 对该实现控制流的审查，不是所有 API 的穷举证明 |
| 真实回归源码 | 未绑定拒绝、最后 lease、两种 BUSY、身份失败、clone 异常、成功/过期采用 | 与上轮要求逐项对应；不是仅添加空测试名 |
| 前后日志 | 修复前析构回调实际修改源/历史/observed，修复后全部不变 | Windows/MSVC 记录；不扩大为全编译器独立复现 |
| 工程记录 | 64/64；原 59 项及断言保留；七组消费者 8/8；显式 P03 | 作为本次放行证据，非最终跨平台、性能或人工 IME 资格 |

## 2. P03 R1 的具体复审结论

### 2.1 输入清理不再依赖两个参数的寿命次序

`PreparedMaterialReload::prepare()` 在函数体入口构造局部 `ReloadInput`，成员次序为 `code`、`source`。函数参数立即被移动到这个拥有单元，节点清理与代码保活建立了明确关系。

这里是函数内类型，没有新增公共框架、安装头或构建目标，也没有使用该持有器的移动赋值。

### 2.2 获得 READING 后，先转移所有权，再检查 binding

`withRead` lambda 的第一条语句是 `auto admitted_input = std::move(input)`。因此未绑定等提前返回路径在 lambda 返回前清理输入；ReadScope 之后才退出。

成功调用 create 时，移动 source，但保留局部 code 强引用并向 create 传入 lease。继续复用 create 的 `InputRelease` 和原身份、History、loaded 基线校验，不用删除拒绝条件取得成功。

### 2.3 准入本身失败时，不擅自解除原状态

若外层已经 CLOSING/READING，lambda 不执行，外层 owning input 仍负责节点先于 code 清理。失败不修改别人的 gate，也不引入第二组 busy 状态。

### 2.4 前后证据与断言相符

相同的“未绑定、外部仍持有 code”场景：

| 观察项 | 修复前归档 | 修复后归档 |
|---|---|---|
| prepare 拒绝 | 1 | 1 |
| 析构回调中公开 apply 成功 | 1 | 0 |
| 源不变 | 0 | 1 |
| History 不变 | 0 | 1 |
| observed 不变 | 0 | 1 |
| dirty/绑定不变 | 1 / 1 | 1 / 1 |
| 返回后正常公开编辑 | 未走到成功校验 | 成功 |

最后 lease 测试另行观察每个输入节点的销毁、代码释放时的存活节点数和泄漏；BUSY 测试检查外层状态保持。测试没有依靠一直保留外部强引用来冒充最后 lease 覆盖。

验收说明也明确：本机修复前未观察到代码提前释放，不能把本次原本通过的 BUSY 场景包装成修复前失败。这种记录方式应继续保留。

### 2.5 范围没有扩张

实现差异限于私有重载头、原测试 CPP 及同一 target 的测试登记。History、SessionStore、SessionState、快照生产实现、目录、target/include/package 名称不因本次补正再次调整。

**结论：原阻塞点已经针对性解决，无需再停在 P03 做新一轮泛化重构。**

## 3. P04 的起点、顺序与结束位置

1. 开始时核对实际 HEAD、工作树与祖先关系。`6a4ebe953ad227838147f28c11b2e57cb6d3fb35` 是已审阅前置；有后续用户改动时先解释，不 reset、不覆盖。
2. 读取 P00/P01/P01-R1/P02/P02-R1/P03/P03-R1 的交接索引及当前施工账本。历史证据仍按各自 `implementation_sha` 验证，不改写过去记录。
3. 读取原 P04 文档和本补充。P03 R1 是前置已完成，不复制那次补丁再提交一次。
4. 只实现 Flow 作者模型和本阶段明确的纯算法迁移。保存 IO、编译/链接、运行预览、桌面 UI、全局插件、Lua 执行语义仍按后续责任处理。
5. 最终显式启用 `LUX_EDITOR_MIGRATION_STAGE=P04`，实现和验收记录分开提交，正常推送实施分支，停在 P04。

## 4. P04 必须先分清的三种事实

当前源码 `modules/function/flowforge/include/lux/engine/flowforge/graph/FlowSource.hpp` 已明确：

- `FlowGraph` 是节点及连接等可编辑运行于 CPU 的图结构。
- `FlowSource` 是拥有型捕获/codec 值，保存 ID、名称、节点、变量、连接和导出；不是第二份可写 FlowGraph。
- `FlowSourceEnvironment` 含类型、类、函数、能力、事件等 span/视图，以及 `code_lifetime`；materialized graph 仍借用其元信息。

旧工具的 namespace 级 `Content` 当前保存 `{id, name, FlowGraph}`。因此原 P04 的条件性 `FlowAuthoringSource` 可以用来准确命名这份可编辑内容，但不应再与一份长期可写 FlowSource 双向同步。

**目标只有一份权威可写作者图。捕获值、编码结果、编译产物都是派生结果。**

不能因为名称相近就把 FlowSource 当成一个全能模型；也不能由于 Flow 和 Material 都有图 UI，就让 FlowSession 继承或内部持有 MaterialSession。

## 5. 类型、文件、关系与修改要求

下面是原 P04 类型清单按当前前置整理后的实施对应。新文件是规定的目标，不声称它们现在已经存在。

| 类型/角色 | 目标位置 | 关系、权限和要求 |
|---|---|---|
| `FlowSession` | `editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowSession.hpp` | `final : sessions::IEditSession`；由 SessionStore 独占，组合 SessionState、作者源和唯一 EditHistory |
| `FlowAuthoringSource`（确有需要才增加） | 同目录 `FlowAuthoringSource.hpp` | 复用 `{id,name,FlowGraph}` 的领域表示；没有第二份同步图，没有窗口/编译状态 |
| `FlowReadView / FlowSnapshot` | 同目录 `FlowSnapshot.hpp` | 同步只读借用与拥有型冻结值分开；不能通过 const Node/Pin 间接暴露可写 graph |
| `FlowEditBatch / FlowEditReceipt` | 同目录 `FlowEdit.hpp` | 意图和完成事实分开；整个批次一次历史提交；采用真实领域 ID/值类型 |
| `FlowGraphEdit / FlowVariableEdit / FlowLiteralEdit` | `model/src/edits/` | 复用 EditOperation/PreparedEdit；允许相关私有小类型同文件归组，不为每类另建库 |
| `FlowSessionAccess` | 同目录 `FlowSessionAccess.hpp` | 复用既有 typed access；无 engine/project/panes 服务入口 |
| `FlowSourceEnvironment` | 原 Flow 低层类型 | 注入、校验和保活；不改为全局单例，不将借用 span 的复制当作底层数组拥有 |
| `SessionState / EditGate / History` | 既有 `editor/editing/sessions/`、`editor/editing/history/` | 直接复用；私有 currentContent、withRead、唯一准入/保存基线继续有效 |

成员实际声明顺序要使源、历史 memento 及依赖元信息的值在环境/代码最后 owner 之前销毁。默认移动赋值不天然等于安全清理顺序：只有实际需要赋值时才提供，并针对旧值退休验证，不为“可移动”而增加无用 API。

FlowSnapshot 若只保存深度拥有的 FlowSource 标量/字符串/容器，不必照搬整个 Material 图克隆方案；其是否仍需保留某部分环境或代码，必须按真实可达数据和回调解释。任何仍持节点、RuntimeObject、factory、allocator/deleter 的结果，都须保活它实际依赖的元信息和代码。

## 6. 从已有 Flow 工具迁出什么，暂留什么

实码起点是 `editor/tools/flowforge/pinclude/lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp` 及对应 CPP。下表不授权整目录删除。

| 已有对象/符号组 | P04 操作 | 暂留边界 |
|---|---|---|
| `Content / source_` | 将作者数据语义用于新具体源；源编辑逻辑只保留一份 | 旧产品切换前可仍持自己的旧工作副本；不得与新会话建立双向镜像同步 |
| `environment_ / metadata()` | 纯模型显式注入元信息环境与 lease | 旧产品装配仍可从旧 Context 获取输入，但新模型不得回取 Context |
| `NodeIndex / PinIndex / read_nodes_ / read_pins_ / indexContent` | 只作为派生索引，编辑后更新/失效，候选失败不污染 | 不跨图替换保存失效指针 |
| `VariableEdit / LiteralEdit / TValueEdit / GraphDelta / GraphEdit / ExportsAccess` | 提取到唯一纯编辑实现，旧调用按窄算法适配复用 | 新旧两份大段纯算法不能以 P12 期限为借口并存 |
| `nodes/pins/links/nodeName/nodeOperation/pinName/pinType/nodeLayout` | FlowReadView 的查询能力 | 不携带 UI 焦点、窗口、全局注册表写权限 |
| `setPinLiteral / rename` | 批次中的领域编辑 | 现有类型校验和 NO_CHANGE 规则保留 |
| `insertNode/captureNode/insertFunctionUse/setFunctionSignature` | 节点/函数编辑及捕获 | 签名、pin、调用关系与连接不能半更新 |
| `exports/setExports` | 可撤销的作者导出声明 | 不等于编译 artifact 发布 |
| `variables/addVariable/setVariable/removeVariable/variableReferenced` | 真实变量规则与引用校验 | 不能只靠 UI disable 防止非法删除 |
| `removeNodes/connect/disconnect/moveNodes` | 顺序批次与图布局历史 | 不把持久节点布局误判成临时视图布局 |
| `requestCompile/acceptCompilation/retryLink/compiled/requestPublish` | 本阶段不搬进模型 | 原消费者暂留；P07 的 compilation/publish 责任 |
| `asset_status_/reading_/read_result_/candidate_/saved_history_/save_` | 不整体搬到新 Session | P05/P12 的持久化和应用用例；私有纯候选不得包含异步 IO |
| `ContentElement/GraphElement/content_/close_connection_` | 不进入作者模型 | P10/P12 接管；旧 UI 仍需工作 |

每项迁移在当前账本写出新归属、仍存在的旧调用方、实际已删算法体、暂留适配及 P12 期限。旧 `FlowForgeEditor.hpp/Impl/FlowAssets.cpp` 并非本阶段整体到期，不得为消灭引用而删除现有产品功能。

## 7. 必须继承的正确性契约

### 7.1 关闭身份与保存基线

实现已存在的私有 `currentContent() const noexcept`，只从 SessionId 和 History 的真实当前状态取得内容戳。不能为了关闭复制 describe、分配字符串或保存第二份 current。

current、observed、保存 checkpoint 是不同事实。Undo 回保存状态与“没有发生通知”不是一回事；新 Flow 模型不增加一个手动同步 dirty 标志。

### 7.2 顺序批次与引用校验

节点删除/替换/重建后，不得保留旧节点的字段准备值和 pin 指针继续覆盖新节点。变量/函数签名/导出/连接的组合必须遵守原 Flow 领域定义，不能把 Material 的端口类型规则直接搬来。

一个批次成功时一次历史提交；失败时完整作者内容、history、observed、绑定和 checkpoint 保持。需要统一修复引用还是直接拒绝，以已有定义和原 P04 明确的策略为依据；原语义未明确的情形应在本阶段先记录设计决议与测试，不能悄悄丢边或改语义。

### 7.3 读取、回调与拒绝输入清理

只要读取过程实际执行 materialize/capture/codec/元信息工厂等扩展回调，就用已有 `withRead()` 保护整个必要作用域。不能只在函数开头检查 available。

回调内重新进入 apply、Undo/Redo、关闭准备或采用，应遵守同一 gate，错误/普通异常清理后恢复。不要把既有测试中的私有绕过 gate 故障注入抄成正常业务接口。

本次 R1 特别要求继续遵守：

1. 需要被消费的输入图/变量/动态值与其环境、代码必须形成明确拥有关系，不依赖独立参数的销毁先后。
2. 准入成功后，在任何提前返回之前，使这些待清理输入位于准入作用域内。
3. 准入本身拒绝时，仍保证值先于最后环境/代码 owner 清理，并且不擅自解锁已有 READING/CLOSING。
4. 测试同时覆盖“外部仍持有代码”和“输入仅剩最后 owner”，不能用前者代替后者。

不要求因此在 P04 新建完整重载服务、IO Operation 或公共 SourceEnvelope。对本阶段实际实现的创建、候选、编辑输入及捕获清理应用上述契约即可。

### 7.4 环境寿命不是只保住 DLL

当前 FlowSourceEnvironment 含 span；即使 code_lifetime 尚在，若其指向的描述符数组由另一个对象提前释放，单独持有代码也不足以维持数据有效。

构造时解释谁拥有 span 的底层数组、各 descriptor、factory、RuntimeObject 的类型与 deleter；用真实对象的销毁顺序验证。没有证据就不能声称“浅复制环境已经获得完全拥有”。

## 8. P04 内部实施顺序

### A：核对实际数据与消费者

读取 FlowGraph、FlowSource、FlowSourceEnvironment、materialize/capture/validate 入口及旧编辑实现。确认可写图、冻结值、元信息借用的边界；从当前 P03 基线提取原 Flow 测试与功能索引。

### B：建立最小独立模型和真实图

建立 `flowforge_model` 和 FlowSession；使用真实小图与真实环境建立变量、函数、连接、导出和布局。最早的测试不需要 UI、编译器服务或链接器。不得先放一组永远 Unsupported 的空 API。

### C：编辑原子性、历史及冻结读取

复用既有 History/SessionState；实现原 P04 中各编辑组、失败回滚、Undo/Redo、冻结捕获。先让完整用例闭合，再判断实际共同算法，避免先发明万能 GraphDocument。

### D：迁出纯算法并删除旧算法体

旧工具只保留窄调用适配和未到期的产品接线。原纯校验/值编辑/图准备实现迁出后同步删除原体；不因旧 UI 尚未切换而保留两套算法。

### E：独立依赖、安装与三会话组合

建立准确的 target/include 闭包检查与真实负例；安装后最小消费者运行真实 Flow 图；将 SceneSession、MaterialSession、FlowSession 放到同一 SessionStore 验证共有协议，完整受影响旧产品继续回归。

## 9. 模块结构与构建要求

继续使用：

```text
editor/
  editing/
    history/                  # edit_history，不重新搬回根目录
    sessions/                 # edit_sessions，不增加第二套 gate/store
  tools/
    scene/model/              # 已完成
    material/model/           # 已完成
    flowforge/
      model/                  # 本阶段唯一新增作者模型包
        include/lux/engine/editor/flowforge/
        src/
          edits/
        test/
      src/                    # 旧产品适配按范围暂留
      pinclude/               # 非新模型依赖
```

`flowforge_model` 依赖允许项按真实图/元信息的纯 CPU 低层库登记，禁止 UI、旧 Context/Editor、保存实现、编译器/链接执行、脚本运行时执行服务及过渡桥的直接或传递依赖。

“脚本的静态类型/事件/能力描述”和“脚本运行时执行服务”不是同一件事；不能只按目录含 script 字样全禁，也不能借类型头之名放进执行器。审查真实 target 与 imported/LINK_ONLY 闭包。

若现有库把純数据和执行服务混在一起，按原 P04 允许先提取真正纯数据子集；不重写执行语义。只有实际出现这种耦合才新增必要低层 target，不为每个 helper、enum、snapshot 分包。

无 linker 的 X04 测试指模型无需调用或发现外部 Flow 链接工具；不是要求破坏原旧产品工具链测试，也不能把整个 Flow 工具关掉来伪造通过。

## 10. 验收映射与防遗漏

保留原编号，下面是观察细化，不重新建立另一套阶段编号体系。

| 编号 | 原 P04 要求 | 本阶段应保留的明确观察 |
|---|---|---|
| X04-01 | 无 linker 的真实 Flow 编辑 | 新建、变量/函数/导出/连接/布局、Undo、冻结捕获可用；独立消费者真实链接并运行，不链接 UI/compiler/执行服务 |
| X04-02 | 引用变量和签名失败原子性 | 引用仍在时删除变量、签名导致非法 pin/边时按确定策略失败或统一更新；失败核对完整源/历史/observed/绑定，成功一次历史提交 |
| X04-03 | 环境长于图/历史/快照 | 外部加载句柄释放后节点、变量值、memento 和必要快照依赖仍有效；拒绝/异常路径最后 owner 顺序正确，输入不泄漏 |
| X04-04 | 三种实际会话同 Store | 创建、发布、正确/错误 typed key、关闭准备和消费均验证；关闭一种不改另外两种；基类没有 compile/play/viewport 等 Unsupported 拼盘 |
| P02/P03 继承回归 | 顺序批次和稳定读取 | 节点生命周期改变后字段基线正确；回调/析构拒绝重入；准入释放后普通编辑可用 |
| 依赖负例 | 真实 CMake/源码检查 | 直接 compiler、经中间目标 UI/Context、旧桥及无法解析依赖被正确拒绝；修复同一夹具后成功，不能把缺包当作命中规则 |

相关 V3 场景为 Q01、Q06、Q09、Q11、Q49，按照本阶段作者域范围报告，不把尚未实施的保存/运行/UI 一并标为通过。

原 64 项测试是本阶段前置索引，保留测试语义与已有失败证据；不是要求机械凑出固定总数。X04 本身必须新增真实行为，不能靠文件名或源码存在性断言替代。

原七组安装消费者继续按影响重新构建/运行；为新 Flow 模型提供安装后真实使用证明。安装消费者不得借源码私有目录或引擎构建目录的未安装产物。

最终验收针对固定实现 SHA：全量受影响构建、第二轮无工作、完整 CTest、SDK 重新安装及消费者、实际架构负例、`LUX_EDITOR_MIGRATION_STAGE=P04`。缺少必测环境时报告 PARTIAL/BLOCKED，不转为 SKIP 或降低断言。

## 11. 保留问题、禁止扩张与阶段末清理

- C01 仍归 P09/P12，C03 仍归 P11，C04 仍归 P12；保留原 FAIL，不顺手修复。
- 新 Flow 模型自身若出现同类寿命/原子性问题，不能挂到旧 C03 等编号延期。
- 原 UI、预览、编译、IO 及唯一 LegacyPersistenceState 桥仍按限定消费者与 P12 期限暂留；不新增永久双架构开关。
- 不实现 P05 文件写入协调、P07 FlowCompilationService、P10 FlowView 或 P12 整体产品切换。
- 声明、定义、原调用、friend、CMake/安装/生成输入、示例及活动测试一并清理；改扩展名、注释副本、forwarding alias、EXCLUDE_FROM_ALL 都不是删除。
- 为保持产品功能暂留的 source/history 是旧工作副本，不要把它和新 FlowSession 镜像同步后宣称统一。

## 12. 可直接交给实施方

> P03 R1 复审通过，允许进入 P04，仅执行 P04。
>
> 从 codex/editor-redesign-v4 当前验收提交 6a4ebe953ad227838147f28c11b2e57cb6d3fb35 继续，核对工作树与祖先关系，不 reset、不覆盖用户改动、不修改 main。
>
> 读取原 P04_flowforge_authoring.md、本启动补充及当前施工账本。沿用 .internal/editor-redesign/ 和 dev_log/P04/；原阶段快照不改写，历史核验按各自实现 SHA。
>
> 实现独立 FlowSession 与真实 Flow 作者图，区分可写 FlowGraph、冻结 FlowSource 和借用元信息的 FlowSourceEnvironment。复用唯一 History/SessionState/SessionStore、私有 currentContent 和 withRead；不包装旧 FlowForgeEditor，不继承 MaterialSession，不新建万能图文档。
>
> 新模型只放在 editor/tools/flowforge/model/，使用 editor/editing/history/ 和 editor/editing/sessions/。不恢复根 history/sessions，不为每个类型新增库。
>
> 迁出变量、字面值、函数签名、导出、连接、节点布局和图编辑纯算法并删除原体；旧 UI/编译/IO 仅留窄适配及 P12 期限。环境、节点和 memento 的寿命要闭合；所有实际执行扩展回调的读取与拒绝输入清理继承 P03 R1 契约，不能新增平行 busy。
>
> 执行 X04-01～04、相关 Q、原 P01/P02/P03/R1 回归、顺序批次和读取/清理负例、真实依赖负例及安装后消费者。三种实际会话需在同一 Store 验证，不用 Fake 或关闭整个工具替代。最终显式使用 P04 门禁。
>
> C01/C03/C04 保持原失败判定与责任；新 Flow 缺陷不得挂旧编号延期。实现与验收分别提交、正常推送，交付实际迁出/删除/暂留表和真实日志，停在 P04 等待复审，不自动进入 P05。

## 13. 固定源码与规范依据

源码均固定到本次 HEAD，不随分支漂移；以下链接用于解释审阅依据，不表示已独立执行相应脚本。

- [P03 R1 验收说明](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/dev_log/P03-R1/README.md)
- [R1 实现提交](https://github.com/LUX-YU/lux-engine/commit/90d74819a358ac722c002fd544b88cb4fbb454a1)
- [PreparedMaterialReload](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/editor/tools/material/model/src/PreparedMaterialReload.hpp)
- [MaterialSession::create](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/editor/tools/material/model/src/MaterialSession.cpp)
- [真实 Material 回归](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/editor/tools/material/model/test/material_session.cpp)
- [修复前未绑定拒绝日志](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/dev_log/P03-R1/before/reload-unbound.log)
- [修复后同场景日志](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/dev_log/P03-R1/logs/material-reload-unbound.log)
- [最后 lease 日志](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/dev_log/P03-R1/logs/material-reload-unbound-lease.log)
- [外层 READING 日志](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/dev_log/P03-R1/logs/material-reload-reading-lease.log)
- [64 项 CTest 归档](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/dev_log/P03-R1/logs/ctest.log)
- [冻结证据核验脚本](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/dev_log/P03-R1/check_receipt.py)
- [当前 FlowSource 与 FlowSourceEnvironment](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/modules/function/flowforge/include/lux/engine/flowforge/graph/FlowSource.hpp)
- [当前旧 FlowForgeEditorImpl](https://github.com/LUX-YU/lux-engine/blob/6a4ebe953ad227838147f28c11b2e57cb6d3fb35/editor/tools/flowforge/pinclude/lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp)

规范依据为本会话既有 V4 `phases/P04_flowforge_authoring.md` 和 P03 R1 复审文件。新约束中有关输入清理的细化，继承已经验收的 R1；对 Flow 未来实现的要求不是对当前尚未存在的 FlowSession 作事实断言。
