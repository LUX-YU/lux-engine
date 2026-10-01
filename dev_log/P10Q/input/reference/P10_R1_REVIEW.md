# P10 复审与 R1 定向补正：UI 草稿和排队操作的内容来源

**日期：2026-09-30**  
**仓库：LUX-YU/lux-engine**  
**实施分支：`codex/editor-redesign-v4`**  
**审阅实现：`11de2c1fee9d477daaa2d26b07911306ea9d6b9f`**  
**独立验收 HEAD：`b9b856477755a9247a7a6810fa880ae62151f565`**

> **结论：认可 P10 主体迁移，暂不放行 P11。仅执行本次 P10 R1。**
>
> 补正主题只有一个：工具 UI 不能把来自旧内容状态的属性草稿或排队意图，重新标记成执行时的最新内容，然后绕过原模型的冲突校验。
>
> 本文中的失败路径来自已读取的生产源码及测试源码。**本审阅环境没有运行新增真实 UI／SDK 负例，没有重跑引擎、GPU、CTest 或完整归档验证器。** 下文的测试是实施要求，不是已经取得的运行成绩。不得把推导、代码草图或这份文档本身记为修复前运行证据。

## 1. 阶段事实与本轮边界

验收提交的直接父提交为上述实现，远端实施分支指向上述验收 HEAD。[S01]

已读取官方 ViewHost、DesktopShell、SceneView 的主要实现，MaterialView、FlowView 的交互与更新路径，InspectorFields，GraphCanvas，原 FlowInteraction／FlowSession，代表性真实 UI 测试、P10 GPU／原生输入日志和归档检查器。[S02–S14]

应当保留的成果：

| 已有能力 | 本轮判断与边界 |
|---|---|
| 正式 ViewHost | 有界槽位独占 DetachedView，真实 Root 准备／提交，后批请求与关闭 BUSY 保留；不是另一个 Session owner |
| DesktopShell | 组合 Root／Presentation／Host；不再借旧 EditorContext 才能组成新桌面 |
| 三工具及辅助 UI | 正式新模块承担功能，生成 Inspector、项目和任务使用具体能力；不是要求本轮重新迁一次 |
| Scene 双视口 | 归档日志记录真实新 SceneView 的像素回读、左右高亮隔离、拾取、资源退休和零验证层错误；不只是两个旧 Pane |
| 原生输入 | 归档记录实际 Windows 输入到新桌面和生成 Inspector 的流程；system IME 仍 NOT_RUN |
| 原成绩 | 185/185 CTest 已读取；用户报告和验收门禁记录 PLAYER 11、14 组 SDK 至少 97 项及原负例；这些是已有场景成绩，不是本次独立重跑 |

**IME NOT_RUN 不是本轮新增阻塞。** 原 P10 启动补充 §10.1 允许把系统 IME 实测与受控／原生输入范围分开报告。本轮不把 Unicode SendInput 当作系统 IME，也不将未测项改为 PASS。现有新 GPU 和原生输入要求继续有效。

本轮不修改 main，不实施 P11/P12，不重做目录和构建边界。保留 ProjectBuilder.cpp 用户修改及原阶段快照。C01、C03、C04 保持各自原失败契约；新的 UI 来源错误不能挂到旧编号延期。

## 2. 问题—不变量—类型—模块的对应关系

| 发现 | 被破坏的不变量 | 精确替代责任 | 模块／修改位置 |
|---|---|---|---|
| F10-01：Flow 的 `properties_` 保留旧节点值，但 `properties_base_` 没有用于采用校验 | 属性值与其来源 ContentStamp 必须作为一个整体，刷新显示不能替草稿更新来源 | 具体节点属性草稿拥有“节点值＋捕获戳”；Apply 采用该戳，Revert 才重新捕获 | `editor/tools/flowforge/ui/src/FlowView.cpp` |
| F10-02：Material／Flow 的 `CanvasRequest` 没有保存显示来源，BEGIN 在延迟执行时读取当前内容 | 一个已经由旧画面产生的意图，其来源不能因排队或 BUSY 等待改变 | 工具层的有界请求保存不可变 `based_on`；执行前复核，不在通用 widget 塞 Session | 两个 `ui/src/*View.cpp` |
| 测试仅通过 `beginEdit/previewEdit/commitEdit` 传入新 edits | 模型的冲突能力不自动证明 UI 草稿／队列正确使用它 | 必须穿过真实属性缓存和实际按钮／画布入口，观测作者、历史及草稿 | 原新桌面测试及 desktop-views SDK 消费者 |

这三行是一条逻辑链，不是三个要新增的子系统。**不新增通用 DraftManager、PendingEditManager、全局事件总线或第二套 History。**

## 3. F10-01：Flow 属性值与内容戳脱节

### 3.1 当前源码事实

文件：`editor/tools/flowforge/ui/src/FlowView.cpp`。[S03]

当前 `Impl` 分别保存：

```cpp
Display display_;                       // 含最新显示快照的 content
std::optional<FlowSourceNode> properties_;
sessions::ContentStamp properties_base_;
```

`select()` 从源读取节点副本，赋给 `properties_`，随后执行：

```cpp
properties_base_ = display_.content;
```

但已读完整文件中，`properties_base_` 只有声明和这处赋值，没有在 Apply、队列准入、执行或重载判断中消费。

`Properties::draw()` 里的两个真实入口直接把旧属性值放入 `edits_`：

```cpp
FlowSetSignature{node.id, node.name, *signature}
FlowSetLiteral{pin.id, pin.literal}
```

之后 `maintain()` 仅检查：

```cpp
owner->get().describe().current == display_.content
```

再把 `edits_` 放入 BEGIN 队列。该检查针对的是 **display**，不是 **properties**。

与此同时，`maintain()` 尾部在源变化且 interaction 没有 overlay 时刷新 `Display`。`install()` 只调用 `graph_.setGraph(...)` 并替换 `display_`，不会更新、清除或标记旧 `properties_`。

### 3.2 可到达的错误顺序

使用同一会话中的一个合法节点和仍然有效的 PinId，不依赖多线程、无效指针或插件析构回调：

1. 内容处于 S0，视图选中节点，得到节点属性副本和 S0 来源。
2. 用户在属性面板修改临时 literal／signature，尚未 Apply；作者内容不变。
3. 另一视图或同一模型的合法编辑，将相同字段提交为 S1。
4. 让原视图执行一次正常更新：画布显示快照变成 S1，旧节点属性副本仍保留。
5. 用户按旧面板的 Apply literal 或 Apply signature。
6. `live == display` 都是 S1，检查通过。
7. 队列的 BEGIN 调用 `FlowInteraction::begin()`，创建 expected=S1 的新批次。
8. 模型收到的是“旧草稿值，expected=S1”，因而不能据原有 stale 检查拒绝它。

说明性数值例子：S0 字段为 10，用户临时草稿为 20，其他视图已经提交 30；旧草稿在没有报告来源冲突的情况下以 S1 身份覆盖 30。数值只是解释，不是实际运行记录。

**模型并没有失去冲突校验。** 原 FlowInteraction 在 begin 时捕获当前戳，原 FlowSession 在 apply 时严格检查 expected。错误是 UI 在两者之前遗失了旧草稿的 S0 来源。[S06、S07]

### 3.3 影响范围

这会导致新提交被旧 UI 草稿无提示覆盖，或者函数名／签名整体值退回旧组合；不能把它描述成已复现的崩溃或磁盘损坏。Undo 也许仍能撤销错误编辑，但这不使其来源语义正确。

History 换代、关闭重绑和删除后重新出现相同数字身份还必须测试，不能只比较裸 NodeId／PinId。原引用和高水位机制不替 UI 自动识别“当前这个副本属于旧 History”。

### 3.4 不是要求自动合并

本轮沿用原 P08/P10 的保守策略：来源不匹配就拒绝，保留可解释的草稿和状态，允许用户显式 Revert／重新选择。**不引入字段级自动 rebase、最后写者获胜或无提示强制覆盖。**

## 4. F10-02：Material／Flow 的排队 BEGIN 同样不保留来源

### 4.1 当前结构

两个 View 的私有 `CanvasRequest` 都只保存：

```cpp
widgets::CanvasEdit input;
std::vector<VConcreteEdit> edits;
ECanvasStage stage;
```

画布 edited 回调接收操作并压入队列，但没有保存操作所依据的内容戳。执行 BEGIN 时直接调用 `view_.beginEdit(...)`，后者从当时的会话读取 current。[S03、S04]

两个文件中 `live == display` 的检查位于 `!edits_.empty()` 的属性 staging 分支；**纯画布事件队列本身没有这道检查**。

### 4.2 必须验证的顺序

1. Material／Flow 的 GraphCanvas 显示 S0。
2. 实际画布操作生成基于 S0 的绝对位置、连接或删除意图，已进入工具队列，尚未执行 BEGIN。
3. 另一条正常作者操作提交 S1；也可以先遭遇临时 BUSY，再在重试前发生 S1。
4. 当前队列在 BEGIN 时以 S1 建立手势，旧意图被当成针对新状态的操作。

这里的重要边界是 **排队接收至真正进入 interaction 之间**，不同于“一个已经建立的 S0 手势在 commit 前被 S1 改变”的老测试。后者原 interaction 能拒绝，前者尚未携带 S0。

本项同样是源代码推导，未在本环境运行实际图控件负例。测试应选会覆盖作者新结果的具体操作，例如对同一节点的绝对位置修改，避免用无冲突的无关字段操作混淆判定。

### 4.3 Generic widget 不应认识 ContentStamp

`GraphCanvas` 的 CanvasEdit 是纯 UI 操作，保持其对 Session、History 和具体模型无知是正确的。[S05]

应在 **MaterialView／FlowView 将 widget 事件转换成领域请求的边界** 保存显示来源，而不是给 `editor/widgets` 新增对 sessions／models 的依赖。也不能用执行时最新 `SessionInfo` 填充这个字段，那仍是同一问题。

## 5. 替代类型与状态关系

### 5.1 Flow 节点草稿：组合值与来源

建议将当前两枚松散成员改为一个私有拥有值，名称可按现有风格调整：

```cpp
struct NodePropertiesDraft final
{
    sessions::ContentStamp based_on;
    lux::flowforge::FlowSourceNode value;
};
std::optional<NodePropertiesDraft> properties_;
```

这是 **具体 UI 私有值**，不继承任何 Session，不拥有 History，不安装成新公共框架。

创建／Revert 时，节点副本和 `based_on` 必须来自同一次稳定读取／捕获。不能读取了最新节点，却继续给它配旧 `display_.content`。优先复用现有捕获和读取接口；没有必要新增一份“当前内容”的权威。

FlowSourceNode 是当前拥有型快照值，本轮不因增加戳而机械加入无用的 CodeLease。Material 的节点副本包含动态节点和代码责任，继续遵循其原拥有与清理顺序。

### 5.2 领域输入队列：每项保存不可变的 based_on

在各具体 View 内，将现有请求项补齐来源；可继续使用原队列和 stage：

```cpp
struct CanvasRequest final
{
    sessions::ContentStamp based_on;
    widgets::CanvasEdit input;
    std::vector<VConcreteEdit> edits;
    ECanvasStage stage;
};
```

上例 `VConcreteEdit` 是说明性占位，不是应引入的公共模板或生产类型。Material 使用既有 `VMaterialEdit`，Flow 使用 `VFlowEdit`。

画布事件的来源取其正在展示的那份内容；属性 Apply 的来源取属性草稿的 `based_on`。不能把来源不同的两批数据无条件拼进裸 `edits_`，再选择其中一个戳覆盖另一批。

原 `edits_` staging 可以改为一个具体的带来源批次／保留来源的入队函数，或直接进入已有请求容器。二选一即可，不能长期保留 stamped 和 unstamped 两条都可提交的生产路径。

### 5.3 三种状态的区分

| 状态 | 谁是权威 | 能否随一般画布刷新改变 |
|---|---|---|
| 当前作者内容 | 原 Session／History | 只能由原模型编辑改变 |
| 当前显示快照 | View 的展示缓存 | 可以正常刷新 |
| 某个草稿／输入的来源 | 该草稿／请求项 | 不可以；只有显式重新捕获才建立新来源 |

`based_on` 是一个历史观察事实，不是第二份 current、dirty 或 busy。它与作者权威没有竞争关系。

## 6. 执行与清理契约

### 6.1 在进入手势之前校验真实来源

处理 BEGIN 前，对当前实际绑定的会话和完整 ContentStamp 进行核对。通过后沿原 interaction begin／preview／commit；其产生的 gesture.expected 必须与请求 based_on 一致。

优先在既有工具层实现，不必重写 interaction。**不要持有 withRead 再调用 begin/apply**，那会与原 gate 契约冲突。稳定捕获用于取得值与戳；真正编辑仍走原编辑准入。若所属具体接口需要极小的 expected 参数扩展才能可靠表达这条关系，应明确替换原入口及所有消费者并补回归，不创建第二套交互流程。

### 6.2 错误必须准确分类

| 情况 | 必须行为 |
|---|---|
| BUSY／暂时无法访问 | 保留原请求、戳、阶段、输入 owner；不重取 latest 后伪装成新请求 |
| 源或 History 已变化 | 返回明确 STALE_CONTENT／相应来源冲突；不调用领域修改，不无提示重基 |
| 会话代际失效 | 按已有 STALE_SESSION 契约取消旧输入，不触及复用槽的新会话 |
| 用户显式 Revert／重新选择 | 重新稳定读取值与戳，然后才能提交新来源的输入 |
| 真实业务校验失败 | 保留原失败事实，不在每次 update 中伪造新的 begin 重试 |

### 6.3 不把准确拒绝变成队列永久挂起

来源冲突是终态，不是可重试 BUSY。受影响草稿可保留供用户查看／丢弃，但前端必须有可用的显式 Revert／Cancel／重选路径；不得让 stale 队首在每帧自动重试并永远挡住 UI。

沿用现有有界容器，必要诊断留在现有 status／具体请求中；不增加无限失败历史。用户的其他待办如何保留或取消要明确，不能为了清一条 stale 操作静默清空新绑定的合法输入。

### 6.4 错误清理仍遵守原 owner 与 gate

Material 动态节点、字段临时值、旧预览 payload 的清理继续在原允许作用域内进行。活 Session 的 BUSY 不夺取或释放输入；明确旧身份失效才走旧身份清理。

不要删掉 Material 现有 `draft_base_` 检查来“统一”成 Flow 原错误实现。InspectorFields 已有针对活动草稿的来源判断，也不应为本轮一致化而放宽。

## 7. 文件、函数、成员处置清单

| 位置 | 必须修改或核对 | 删除／禁止保留 |
|---|---|---|
| `editor/tools/flowforge/ui/src/FlowView.cpp` 的 Impl | 将节点属性值与其来源组合；display 刷新不更新旧草稿来源 | `properties_base_` 只有写入无消费的旧形状；仅换名不算修复 |
| 同文件 `select()`／REVERT | 值和戳从同次捕获取得，失败保留原可解释状态 | 给新节点副本配旧 display 戳 |
| 同文件 Properties::draw Apply literal/signature | 调用实际共享的有来源入队入口 | 裸旧值塞入 staging、执行时补当前戳 |
| 同文件画布 edited 回调／CanvasRequest／maintain | 事件接收时固定来源；BEGIN 前检验；拒绝后可恢复 | 从执行时间读 current 后给旧事件贴新标签 |
| `editor/tools/material/ui/src/MaterialView.cpp` | 同样补画布队列来源，保留当前节点草稿的已有保护 | 第二条 unstamped queue/转换旁路 |
| 同两文件的 edits staging | 不同来源不能被合并为一个无来源 vector | 同时保留 stamped 与未 stamped 的两套提交路径 |
| `editor/tools/scene/ui/test/views.cpp` 与适当的测试辅助文件 | 将真实控件／草稿／队列边界纳入测试 | 只构造新 edits 调 begin/preview/commit 的替代负例 |
| `editor/tools/scene/ui/CMakeLists.txt`／`cmake/installed-consumers/desktop-views/CMakeLists.txt` | 登记新的真实场景，native 和安装 SDK 使用同一正式实现 | 为让负例运行而导出 Impl 或挂全局 friend |
| 工具 UI 契约说明、施工账本 | 写清草稿、显示及基线语义；更新删除和消费者记录 | 原问题仅加 TODO 或变为“调用者必须及时刷新” |

可在原测试 target 下添加一个专用测试源或私有测试辅助文件；这不是新增生产库。不要以“必须零新文件”为理由把测试塞成另一份难读的大型生产 Impl。

生产路径原则上限定两个 UI CPP。确需调整正式控件操作方法声明时，只增加实际 UI 与外部合法消费者确实需要的窄操作，不为测试安装后门。

## 8. 不允许触碰的边界

本轮不改变以下算法／所有权：

- SessionStore、SessionState、History、原三模型的 current/dirty/checkpoint 和 P04 高水位。
- WriteCoordinator／SaveService／SaveExecution、编译任务和 TaskRuntime。
- ViewHost／DesktopShell 的关闭、挂载与渲染 Runtime 协议，除非真实新负例明确指向它们；不得顺手重写。
- GraphCanvas 的领域无关性；widgets 不依赖 SceneSession／FlowSession。
- Workspace 读取、布局纯计划、迁移 selected 范围和已存在 recovery/marker。
- 原目录、包名、生产 target／DLL 数量。

不添加通用 rebase 系统。严格来源拒绝满足本轮最低目标；更智能的合并属于以后独立论证，不设为 R1 完成前提。

## 9. 六组实际回归与修复前证据

详细输入顺序、观察和反作弊要求见 [TEST_PLAN.md](TEST_PLAN.md)。六组分别为：

| 编号 | 最少覆盖 |
|---|---|
| R10-R1-01 | Flow 旧 literal 草稿＋另一路 S1＋显示已刷新＋真正 Apply 入口；不能覆盖 S1 |
| R10-R1-02 | Flow 旧函数签名／名称草稿；不还原另一操作更新的完整签名与关联内容 |
| R10-R1-03 | History 换代、节点／Pin 数字相同、关闭或重绑定；旧属性不能写新身份 |
| R10-R1-04 | Material 与 Flow 画布事件排队于 S0、BEGIN 前 S1；完整戳没有被刷新替换 |
| R10-R1-05 | BUSY 前后保留输入，再按同戳成功或变戳拒绝；cleanup、容量、状态恢复 |
| R10-R1-06 | Revert／重选与正常新输入成功，一条历史、Undo/Redo；完整既有回归 |

**修复前先测再改。** 原 P10 成绩保留；用 `11de2c1f…` 的生产代码叠加新测试或原安装 SDK 取得真实负结果，再修改生产实现。新负例如果没有按推导失败，应先记录实际原因并修正审阅假设，不伪造输出或降低断言。

## 10. 测试入口必须跨过真正问题边界

现有 `flowView()` 功能矩阵把新构造的领域 edits 传给 `beginEdit/previewEdit/commitEdit`，证明这些接口可用，但没有从旧 `properties_` 读取值，也没有经过 Apply 按钮到队列的路径。[S08]

新增测试必须满足：

1. 节点属性由正式 UI 的选择／读取路径建立；不能仅在测试局部保存一个 FlowSourceNode。
2. S1 后至少经历一次正式 View 更新，使 display 已经刷新；否则仅靠现有 display 检查就可能拒绝，测不到缺陷。
3. 实际 Apply 按钮或它和测试共用的正式窄处理函数消费原草稿；不得改成测试自己提交带旧 expected 的批次。
4. 画布负例必须先经过真实 GraphCanvas 的 edited 接线或其正式事件处理入口，再延迟 BEGIN；不是测试自己调用 interaction.begin。
5. 记录 expected、live、display 三种关系及作者结果。日志可以记录试验观测，但不为打印值在生产中增加一套权威状态。

可复用当前原生输入 harness 或增加小范围、生产 UI 同样使用的具体操作函数。禁止 `#define private public`、把全部 Impl 导出给 SDK、另造 FakeFlowView 或在测试中重写修正算法。模拟时间顺序和实际操作状态应分开说明。

## 11. 验收矩阵与证据口径

保持原 185 项行为，必要测试扩展不删断言；重跑 PLAYER 原 11 项、原 14 组 SDK 97 项及新增场景、依赖负例和八项 operation 编译负例。

新 SceneView 双视口 GPU／像素回读、原生输入、GPU_UI 与 EDITOR_SCENE_PANE 两个旧显式模式继续运行。准确记录实际命令、模式、设备和验证层错误；不能把目录名作为模式证据。

系统 IME 候选／组合／提交如未测，仍写 NOT_RUN。本轮不要求仅为改判定而写 PASS，也不把它扩充成修正草稿语义的前置。

门禁显式 `LUX_EDITOR_MIGRATION_STAGE=P10`；实现／命令／日志绑定最终实际测试 SHA。原 P10 和之前 frozen 证据保持；当前测试源码可以按明确行为映射扩展，不强求新增测试文件数等于新增场景数。

须保留：原错误输入、修复前结果、修复后结果、实际 native／SDK 编译及运行身份、测试模式。只要源码推导未实际运行，就写 NOT_RUN，不以本包文档或自检成绩冒充产品运行。

## 12. 交付与停点

1. 先核对当前分支和用户修改，不 reset、不 force-push、不修改 main。
2. `.internal/editor-redesign/` 仍是唯一可变施工材料；没有第二份新的 current 账本。
3. 完成 UI 补正、原算法体/失效成员处置、真实回归与安装。
4. 实现单独提交；`dev_log/P10-R1` 收据、命令、文件清单、源/测试映射和已知限制随后独立提交。
5. 正常推送并停止在 P10，等待复审，不自动进入 P11。

可直接发送给实施方：

> P10 主体迁移认可，暂不进入 P11。本轮只补正 UI 草稿和排队输入的内容来源：Flow 属性副本不能在 display 刷新后以最新戳提交；Material/Flow 的排队画布 BEGIN 必须保留输入产生时的来源。
>
> 先运行真实新 UI／SDK 负例并保留结果。将属性值与捕获戳组合，队列项保存不可变 based_on；执行前核对真实会话，BUSY 保留，STALE 明确拒绝且提供 Revert／取消恢复。不能执行时补 current、无提示 rebase，或通过重新开始手势掩盖冲突。
>
> 模型、History、SessionStore、执行器、Host、GPU 和 Workspace 保持原责任。只在具体 UI 处理和现有测试中落地，不新增通用 Manager/队列/库，不让 widgets 依赖作者模型。失效的 properties_base_ 松散形状和无来源提交旁路不得残留。
>
> 保留原 185 项、PLAYER、14 组 SDK、operation 编译负例、实际新双视口与旧显式 GPU 模式。系统 IME 未测保持 NOT_RUN。显式 P10 门禁，保护 ProjectBuilder.cpp 和历史快照，C01/C03/C04 保留原责任。实现和验收分别提交并推送，停在 P10。

## 13. 审阅范围

本次不是全仓每个新增成员的逐项 AST 审计，也没有把整份 195 文件变更均已独立执行作为结论。完整已读路径和对应来源见 [SOURCE_INDEX.md](SOURCE_INDEX.md)，机器可读范围见 [review_scope.json](review_scope.json)。

**本包文件生成、链接检查和 ZIP 完整性检查只证明交付文件可读取，不证明引擎代码通过。**
