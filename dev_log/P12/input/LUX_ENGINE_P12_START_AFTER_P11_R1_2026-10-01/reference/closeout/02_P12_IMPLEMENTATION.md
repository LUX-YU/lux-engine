# P12 — 完整产品接通、唯一入口切换与残留清零

本文件只有在 P11 实施与复审完成后才开始执行。遵守 `00_MASTER.md`，以实际 P11 验收 SHA 为基线，不 reset 到本文引用的旧 SHA。

> P12 的完成标准是用户运行的正式 Editor 已经使用新系统，旧框架没有活动消费者并已删除。集成 harness 能运行、不代表产品已切换；删除原文件夹、不代表原责任已迁移。

## 1. 本阶段出口

- 原有适用功能全部有正式新产品入口，并有行为映射。
- Open/Reload/SaveAll/Close/Exit/Layout/Restore/模型插入的组合语义完整。
- C01、C04 对应契约在新产品实际通过，P11 的 C03 继续通过；历史失败原样保留。
- 现有 `lux_editor` target 只定义一次，定义位置归 `editor/application`，不再链接旧 editor_app/context/ui/三大 Editor。
- launcher 保留其真实启动和创建项目功能，但不再是旧 Editor 的另一入口。
- `editor` tracked 一级目录只剩 `editing, authoring, activities, workbench, application, tests`。
- 旧 source、声明、friend、TestAccess、导出、生成器输入、CMake 入口、空转发 target、包 alias 和可运行旧产品路径同时清零。
- 正式保留的 engine/modules 能力、纯数据兼容读取、用户数据和历史记录没有被误删。

P13 不接收以上未完成项。未完成则 P12 PARTIAL/BLOCKED，不能通过修改门槛或改名字收尾。

## 2. 不再建一套 Workflow Framework

用户用例确实需要状态，但不需要共同的 Workflow 基类或通用反射调度器。

- 同步、无在途责任的操作使用自由函数或现有服务方法。
- 跨帧操作用具体 move-only 记录或实际服务内有界记录；每个状态必须对应真实等待条件。
- 根应用组合这些服务，不替各服务保存 source、write outcome、compiler result 等第二份事实。
- 现有业务 IDs 与 TaskId 分开；task cancellation 不等于业务已撤销。
- UI 消失不自动取消应用拥有的内容、保存、编译或运行。

以下类型名是建议的语义分组；可以合并紧密相关的值到同一头，不要求每个值对应单独 CPP、库或类。

## 3. 类型、层与文件责任

| 类型/职责 | 正式位置 | 持有内容 | 不应持有 |
|---|---|---|---|
| `OpenAssetOperation / OpenResult` | `activities/sessions` | 固定地址/工厂、加载结果、发布事实 | Pane/Host、第二 Session owner |
| `ReloadSessionOperation / ReloadOutcome` | `activities/sessions` | 审阅 stamp、旧写入等待、完整新候选 | 旧 Editor、UI 对话框 |
| `SaveAllOperation / SaveAllReport` | `activities/sessions` | 固定 SessionId 集、现有 SaveId 和每项结果 | Pane 集、另一份保存状态机 |
| `CloseSessionsOperation / CloseReport` | `activities/sessions` | 确定内容集合、选择版本、原 ClosePermit | 具体 Pane/Renderer 私有结构 |
| `InstalledSession` / 安装角色目录 | 复用 P11 `activities/sessions` | 角色/注册/关闭能力、SessionId | 源/历史重复所有权 |
| `ModelCreationOperation` | `activities/scene` | 已加载模型、原目标/stamp、参数、一次提交结果 | 视图指针、RuntimeEntity 代替作者身份 |
| `ApplyLayout` 的 UI 准备/提交 | `workbench/desktop` | 固定布局计划、原 ViewId、候选和挂载准备 | 文件 IO、打开内容、保存源 |
| `OpenAndShow / CloseView / RestoreWorkbench / ExitEditor` | `application/src` | 跨内容和工作台的组合进度 | 两边私有状态或万能 service map |
| `EditorApplication / Config` | `application` | 正式服务、相位/总生命周期 | 所有具体操作的共享大状态袋 |
| 未保存/错误/缺扩展提示 UI | `workbench/desktop` | 拥有型问题值、明确用户选择 | 未核验的延迟裸指针 |

构建目标按真实闭包划分，不为每项 Operation 新建 library。已有保存、编译、Run、ProjectStorage 和 WorkspaceStore 的 target 与算法优先保持。

## 4. 需要先补齐的窄 owner 能力

### 4.1 会话枚举

当前 SessionStore 有 size/describe，没有可假定存在的全量枚举 API。SaveAll/Exit 必须枚举会话，不能改为枚举 View。

若 P11 未补足，添加一个 owner-thread `snapshotIds()` 类窄方法，返回拥有的 SessionId 列表和准确错误，不暴露槽位、IEditSession* 或可写容器。按实际命名保持唯一接口。分配函数不无依据声明 noexcept。

枚举结果只固定这次操作集合；每项使用前仍检查代际。新开的 Session 不自动加入既有 SaveAll；退出期间新建请求按明确的应用退出准入拒绝或延期。

### 4.2 一组关闭许可的提交

当前 `close(ClosePermit&)` 单项接口不自动证明多会话原子关闭。

优先复用现有完整准备能力。如果逐项 close 会在第二项失败时已经删除第一项，必须在原 Store 增加窄的“验证全部已准备许可→一次提交集合”能力；不在 application 通过假 rollback 补救。

该能力只能知道 Session/permit/身份，不能 include ViewHost、SaveService 或 RunStore。逻辑删除的可见性与随后对象析构清理分开；析构回调中的 Store 访问继续遵守原 reclaiming 保护。

### 4.3 工作台多视图事务

当前 ViewHost 的 adopt/drain 是单视图/队列能力。完整布局计划和批量关闭要检查是否能满足失败不改原 UI。

只在原 ViewHost/Root 准备协议中补需要的批量候选、路由预留和一次安全提交；不创造第二 DockManager/WorkspaceHost。PreparedViewBatch 是一次性拥有责任，不能复制。

### 4.4 共同原则

准入时保证必需资源/依赖，连续无回调的内部处理复用验证结果；跨回调、异步和外部 IO 后重验可能变化的事实。不要把“少写 if”当作完成标准。

## 5. P12-A：冻结真实功能和清理计划

从 P11 最终提交出发，读取：

- 正式五层的 headers/targets/source map；
- P11 内置命令、SessionFactory、ViewFactory、角色和动态 SDK 的资格；
- 当前旧 `editor/app/product` 菜单、旧三个工具、launcher、settings、旧协议测试；
- `retained-product.json` 经 P11 更新的精确剩余消费者；
- 原 C01/C03/C04 断言与已有真实 IO/GPU/输入测试。

每个适用用户功能记录五列：旧入口、正式新入口、底层唯一 owner、等价测试、旧实现退出提交。不得只列类型名。

必须包含创建/打开/保存/另存为/导出副本、撤销重做、对象/组件/父子/配置、材质/Flow 图功能、编译/重链接/发布、模型插入、作者和 Run 切换、运行检查、资源重试、项目浏览/创建、任务取消、设置、布局/恢复、最后视图和退出。

新机制明确拒绝的非原有能力可记录边界；原已支持功能不能以 unsupported 保留到 P13。

## 6. P12-B：内容操作

### 6.1 OpenAsset 与 OpenAndShow

**内容打开放 E2，显示组合放 E4。** 不让 activities include IViewHost。

状态和提交点：

```text
REQUESTED
  → 校验 project instance / AssetAddress / SessionKind
  → 查已打开或正在打开的同一工作副本
  → 固定 P11 工厂 snapshot
  → Process 读取和 decode
  → owner 构造与完整安装
  → CONTENT_PUBLISHED
  → E4 复用或创建视图
  → PRESENTED / CONTENT_WITHOUT_VIEW / PRESENTATION_FAILED
```

要求：

1. 默认按 `(项目实例, 源定位, 会话种类, 工作副本政策)` 复用，不按 Pane 类型找第一个。
2. 同一内容的并发重复打开合并到已有操作或明确复用；不能发布两个意外工作副本。
3. 需要显式第二工作副本时必须是独立意图，不能由第二视图隐式触发。
4. 单个等待者取消不销毁其他等待者已需要的共同工作；所有已接受工作最终结清。
5. Session 发布前取消/失败不留可查半对象；发布后取消是部分完成事实，不能悄悄删除已发布内容。
6. 新 Session 成功但 ViewFactory/adopt/focus 失败，保留可访问的无视图会话，报告具体失败。用户之后可以重新打开视图或关闭内容。
7. 读到的字节版本与 SourceBinding 一致；源在读入期间变化时按正式版本规则处理，不把新路径和旧内容拼成一个假绑定。
8. Source 与角色 owner 先后依照 P11 安装协议；不把一个 std::unique_ptr 同时交给 Store 和 InstalledSession。

不能用“一次创建 Session 再立即关掉”的补偿来抹掉已经通知外部的发布事实。

### 6.2 ReloadSession

重载保留 SessionId，但建立新的内容/History 域；这与 Save As 完全不同。

顺序：

1. 确认目标会话、来源、未保存内容，以及用户回答所依据的完整 ContentStamp。
2. 识别同目标、旧来源的在途写入。Unknown 仍可能晚发布时不继续采用。
3. 得到可读取的稳定来源，固定工厂，读取/解码新源；不先 clear 原内容。
4. 通过现有具体 PreparedReload/作者内部能力构造完整新源、索引、History、checkpoint 和角色候选。
5. 最终采用点再次核对被审阅 stamp、绑定版本和写入集合。读取期间新增的旧来源写入也必须结清或拒绝本次候选。
6. 所有可能执行 codec/工厂回调的准备结束后，在原 owner 短安全点采用；不能在“最后一次写入核验”和交换之间再调用任意业务回调。
7. 内容变化需新审阅或返回冲突；临时 BUSY 保留候选，不自动换 expected。
8. 成功后旧来源的编译、投影、保存采用按 History/内容身份准确失效；已发生的旧文件发布事实不抹掉。

若现有 WriteCoordinator 缺少所需的在途目标查询，可加窄只读查询/正式协调请求。不要再创建一份文件写队列；不要只等 `Task.finished`。

不要求锁住 UI 直到磁盘完成。允许继续编辑时就必须真正通过最终 stamp 校验；不要在读入初期持长时间 READING 来省掉这个问题。

### 6.3 SaveAll

输入来自 `SessionStore.snapshotIds()`，不是 PaneManager 或 ViewHost。

每项分别记录：无保存角色、未绑定需 Save As、用户取消、准入失败、已接纳、发布 Unknown/成功/失败、基线采用结果。结果引用现有 SaveId，不重跑 encoder、不复制保存状态机。

两个视图同源只保存一次；无窗口但 dirty 的会话也在本次集合。后续新开会话不偷偷加入本次操作。

工作台触发保存时，对当前明确涉及的交互组先完成或取消其未提交手势；冲突/无效草稿不得被静默提交。该 UI 预处理在 E3/E4，SaveService 和 E2 SaveAll 只保存已提交源，不暗中结束其他窗口的编辑。

“所有 request 已被接受”不是“全部已保存”。某项完成后又有新编辑，可以报告该捕获版本已保存、当前仍 dirty，不篡改 SaveReceipt。

### 6.4 CloseSessions

先收集决定，再启动不可逆关闭。

```text
固定 Session 集和原观察
  → 收集带 stamp 的 Save / Discard / Cancel 决定
  → 执行必要保存，保留磁盘真实结果
  → 重验所有最终决定与 current / dirty
  → 准备全部关闭许可、角色撤销、相关 view/run 依赖计划
  → 全部可提交后进入不可逆关闭阶段
  → 原 owner 完成资源退休和 Store 逻辑关闭
  → 逐项/聚合结果
```

在用户决定尚未收齐或第二张许可失败时：不删除任何会话、不关任何视图、不先停止另一个独立 Run。A 已经成功保存的磁盘结果保留，不伪装回滚。

Discard(S10) 不能授权删除 S12。保存 S10 后用户编辑到 S12，不能以刚才保存成功直接通过关闭。

内容关闭时存在 Run：明确询问/政策决定 Stop 或 KeepFrozenRun；选择保留只能在证明它完全依赖冻结源和环境时成立。不能由“关闭 View”隐式推断停止 Run。

内容关闭关联 UI 的具体组合由 E4 负责。E2 返回准备好的内容关闭结果/许可；E3 准备视图解绑；不要让 E2 源码包含 Pane、Root、SceneView。

进入不可逆阶段之后发生的资源失败要返回真实 pending/failed 状态，继续保留足够 owner 安全收尾；不恢复已销毁对象，不让错误直接落入析构。

已准入 Save/Compile/Run/GPU 的完成依照原服务结清。控制关闭责任不得通过一个新的万能 SessionCloseDependencies::get<T>() 实现。

## 7. P12-C：工作台整体操作

### 7.1 Hide / CloseView / CloseSession / Exit

| 动作 | 默认效果 |
|---|---|
| HideView | 只改变可见性，不触发内容关闭。 |
| CloseView | 关闭一个视图，退休它的资源；不关闭共享源。 |
| 最后视图 CloseView | 明确询问 keep session / close content / cancel；回答前不先销毁。 |
| CloseSession | 使用上述内容关闭用例，处理关联视图/运行政策。 |
| ExitEditor | 对固定的项目/内容集合进行总审阅和资源结束，再退出平台。 |

ViewHost 的原 close request 不是产品“已完成全部关闭”的证明。操作记录需要查询真正的 drain/retirement 和关闭错误。

### 7.2 ApplyLayout：真实批量准备，而不是循环创建后假回滚

输入：E2 WorkspaceStore 读到的 DockLayout，E1 纯 LayoutPlan，E3 当前视图观察和固定 ViewFactory 快照。

具体步骤：

1. 完整解析/验证；坏 Dock 数据必须在任何 visibility/focus/挂载变化之前拒绝。
2. 固定当前 ViewId、restore key/type、原配置和相关工作台版本；计划只做精确匹配。
3. 默认保留额外窗口、现有绑定和 dirty 内容；不打开 opaque locator。
4. 一次准备所需 DetachedView、配置校验、Dock 输出、挂载/路由槽、解绑和焦点变更。
5. provider 构造或配置验证失败时，销毁未发布候选，当前 Root、可见性、选择和内容不变。
6. 提交前重验 View 代际和工作台版本；变化则取消或重新规划，不将旧计划应用到新对象。
7. 对受影响交互默认 CancelPreview，按原 gate 完成；未成功前不修改结构。不能暗中提交草稿。
8. 安全点只执行已准备的拥有权/结构转移，不再调用会失败的 provider 创建函数或文件 IO。
9. 提交后通知及偏好持久化单独记录。偏好写失败不意味着布局没有应用；通知产生的结构请求进入下一批。

本用例只保证契约内 UI 结构准备，不承诺回滚任意插件偷偷执行的网络/文件副作用。工厂收到的能力应足够窄，违规行为单独拒绝或诊断。

C01 的核心观察必须覆盖：原窗口 hidden，输入 malformed dock，结果拒绝且窗口仍 hidden、数量不变、原 dock bytes/布局语义不变。

### 7.3 RestoreWorkbench

从独立 RecoveryManifest 经 OpenAsset 恢复内容，再由 E4 创建/复用视图并绑定。固定同一组 P11 贡献快照。

- 不聚合全部历史布局的 locator；继承 P09 R1 selected 范围。
- 不声称只凭资产 locator 可以恢复未保存修改；没有真实恢复快照就明确只能恢复已发布内容。
- 缺 provider/类型/资产时保留未知数据并报告，不能无提示删除清单项。
- 已存在用户修改过的 recovery/marker 不自动覆盖；旧格式只读导入保持幂等。
- 单项失败不隐藏其他项实际已打开的内容；结果区分复用、新建、未恢复和未显示。
- 自动恢复出现错误不能陷入每帧重开无限循环；重试条件和用户明确触发分开。

## 8. P12-D：所有实际工具入口

### 8.1 三种作者工具

继续直接使用已有 Session/Interaction/Preview/Compilation 服务，不重写纯编辑和编码算法。

- Scene：对象创建/删除、父关系、组件增删/字段、配置、相机、模型放置、资源状态和重试。
- Material：图、常量、纹理/参数、持久节点布局、编译、预览、发布。
- Flow：变量、函数与签名、Pin/连接、字面值、导出、布局、编译、固定产物链接重试和发布。
- 所有 UI 草稿/队列保留 based_on，显示刷新不能使旧草稿变新。
- 两个视图共享同一 Session/History；作者/运行目标路由显式，不因焦点变化误写。

### 8.2 运行 Inspector 不得靠保留旧 SceneEditor 继续提供

P10 原报告没有宣称新的运行 Inspector 全部完成。本阶段先从旧功能矩阵确认已有支持范围，再用 RunInspectAccess/原运行编辑能力接入正式 Inspector。

只读检查必须可用。原来支持的运行编辑若属于产品功能，继续经 activities/scene 的真实 Registry 编辑能力执行，并且不写作者 checkpoint/history。不得让作者 Inspector 获得任意可写 Run Registry。

不因 `editor_editing_scene` 名字像旧模块就删掉其正式运行消费者。消除旧 UI 适配，不删除有用的原运行算法。

### 8.3 模型插入

用户触发时固定资产引用、目标 Session/History、内容戳、放置参数。Process 只读入/解码；owner 在实际提交前核验目标和许可，调用一次原 SceneEditBatch 插入。

模型读完但目标已关闭/换 History、预算不足或取消：最终结果不是 Inserted，源不半改。UI 不可通过视图已关闭来丢掉已接纳操作的结清责任。

### 8.4 项目、任务、设置和启动器

项目创建复用纯 ProjectBuilder 与原 ProjectCreation/Storage；AssetImporter 继续在 E2。TaskView 借 TaskMonitor，不抢 Runtime observer。

设置/插件选择保留“下次打开生效”与“立即更新注册”的明确区别；首版不要求任意热重载。重开项目的流程不得自动保存/清除未经审阅内容。

原 launcher 的项目选择/创建 UI 迁到 `workbench/project` 或 settings 的合理位置；其进程启动继续复用 `application/launch/launchEditor`。只作为选择/启动器的独立 exe 可以存在，但不能是第二份旧 Editor。

## 9. P12-E：EditorApplication 与唯一推进

### 9.1 构造失败必须真实失败

`EditorApplication::create(Config)` 返回完整应用或准确错误。步骤至少覆盖原引擎/项目服务、插件依赖、内容角色、工作台、输入与菜单连接、初始布局准备、必要持久化设施。

每一步失败都清理局部候选，不能发布“已构造成功但 outcome 已失败”的对象。

特别保留 C04：菜单连接失败引起 create 返回 `object.connect` 或新准确等价诊断；不把它换成退出失败测试。构造期 Result 和运行期 fault channel 分开。

允许在真正准备好的无业务失败位置使用 noexcept；含分配、插件构造、打开文件和任意 callback 的地方不得未经处理统一 noexcept。

### 9.2 相位与驱动

将目前正式集成使用的真实时序写成一份循环，不让每个 View/RunStore 自己 tick 原 SceneRuntime。

阶段必须覆盖：输入收集、Process 完成吸收、业务结果采用、操作推进、UI 准备与借用结束、唯一 Scene 驱动、Render 提交/采用和安全退休。具体顺序遵守已有 runtime/UI 同步依赖，不机械照抄一行伪时序。

不能停止消息泵后再等待只有该泵才能完成的 task/GPU/dispatch。背压保留原包，不能重复 draw 以重放业务。

通用 activity 不读全局 App。应用装配通过必要引用与 Connections 接线，禁止 getService<T> 或 string service map。

### 9.3 退出顺序与不可逆点

推荐用明确的阶段状态表达，而不是一个 closing bool：

```text
RUNNING
  → REVIEWING（可取消，无提前销毁）
  → COMMITTING_EXIT（决定与许可最终核验）
  → DRAINING（停止新准入，已接纳结果可靠结清）
  → RELEASED（平台/执行器等最后依赖结束）
```

这些是本应用的生命周期，不替各服务复制状态。

进入 DRAINING 后：

- 停止新用户业务准入，但继续接收已准入任务/文件/资源完成。
- 按确定政策结束保存/编译和 Run/预览；Unknown 文件发布未能确认 writer 退休时不能假完成。
- 结束工作台交互、路由、视图和它们的 GPU 责任；窗口和 surface 活到真实退休。
- 撤销保存/历史/扩展角色的未来调用，等待当前回调结束。
- Session/source/history 及仍需插件代码的结果/闭包按实际引用依赖结束。
- 释放代码/插件，再结束必要原平台和执行器；Runtime 必须活得比自己的 Task/Scope/lease 长。

不要将这一列表机械写成固定析构成员顺序；检查真实借用图。析构不弹对话框、不隐式保存、不启动新业务，不强制终止线程来伪造退出成功。

错误不能抹掉已经成功的文件发布、已停止 Run 或部分关闭事实；报告首个原始失败和后续实际清理状态。

## 10. P12-F：切换正式产品

1. 正式新组合先经集成测试和实际 UI/GPU 路径运行。
2. 将当前 `lux_editor` 的定义从 `editor/app/CMakeLists.txt` 移到 `editor/application`；同一阶段只保留一个定义和安装入口。
3. 内置菜单、三工具、插件贡献、项目创建、launchEditor、示例和外部包指向正式新契约。
4. 新产品路径不得 include 旧 EditorContext/PaneManager/三大 Editor/旧 TestAccess；不得用旧 dll 完成隐藏工作。
5. 新 harness 仅作为测试程序，不安装成 NewEditor，也不保留 `--old-editor` 或自动回落。
6. 第一次安装到新空前缀并独立运行，避免旧开发 SDK 的残留补齐未声明依赖。
7. 实际启动已安装 `lux_editor`，证明使用新 bootstrap；仅运行链接新库的测试 exe 不足以证明产品入口切换。

`editor_bootstrap` 是可采用的真实装配 target 名，不是必须新建空转发 target。它若存在必须有实际装配源，不只是链接全部 Editor 库的 INTERFACE 壳。

## 11. P12-G：残留清零

严格依照 `03_REMOVAL_PLAN.md` 逐文件/符号/target 删除。九个旧根目录到阶段结束全部消失：

```text
editor/app
editor/assets
editor/context
editor/launcher
editor/metadata
editor/plugins
editor/tools
editor/transition
editor/ui
```

同时清理隐藏在正式根中的旧协议，例如 `editor/editing` 内的 old editor_editing/AssetSave/CloseRequest，或活动 support 中只服务旧窗口的 TAssetSave/Legacy*。不能只检测上面的目录。

必须保护：

- 正式的 editor_assets/AssetImporter、editor_storage/ProjectStorage、editor_editing_scene/运行编辑、editor_launch；
- History/SessionState/Store、原 codec、纯分区解码及其 Process 复用、renderer/process/对象基建；
- 版本化 LegacyWorkspaceImporter，它是数据格式读取，不是旧产品 owner；
- 用户文件、ProjectBuilder 用户补丁、历史失败/收据和负向样本。

原 legacy 根中的测试有价值则移入实际新测试域后删除旧入口；不能在 tests/legacy 再编译一套旧 Editor 来维持测试数量。

删除时同时处理 `.hpp/.cpp`、声明、定义、调用、friend、生成输入、CMake、source include 根、包/导出、visibility 头、运行库安装、示例、脚本和长期文档。历史 dev_log 中的旧名字应保留，不作全局替换。

## 12. P12-H：最终资格

### 12.1 原 X12 核心含义保留

| ID | 输入/故障 | 必须观察 |
|---|---|---|
| X12-01 | Session 已发布，视图创建/采用/聚焦失败 | 无视图会话仍可用；OpenResult 表达真实部分完成。 |
| X12-02 | 原保存晚发布、Unknown、重载期间新编辑/新保存 | 不采用过时读取；不丢未经审阅内容；失败保留原源和历史。 |
| X12-03 | A 保存后 B Cancel，第二张许可失败 | 所有内容/视图仍在；A 真保存保留；许可释放。 |
| X12-04 | 最后视图 keep/close/cancel | 三种语义明确，回答前无销毁；Run政策明确。 |
| X12-05 | dirty 双视图、额外窗口、缺 provider、坏 dock、偏好写失败 | 内容不变；结构提交准确；偏好失败独立；C01对应断言通过。 |
| X12-06 | 两窗同源、无视图源、无绑定源、新开源 | 按固定 Session 集去重并逐项报告，不假全部保存。 |
| X12-07 | 模型加载成功，提交前目标换代/预算失败 | 最终不报插入成功，作者源原子不变。 |
| X12-08 | 构造每个必要步骤及菜单连接失败 | create 返回失败，没有已发布半应用，C04含义保持。 |
| X12-09 | 编码、运行、GPU、插件回调同时在途退出 | 可靠完成与真实退休；无消息泵先停、无代码提前释放。 |
| X12-10 | 实际 exe/插件/安装/源码/target 扫描和启动 | 唯一新产品；零旧 owner/bridge/回落；五层 + tests。 |

### 12.2 不可省的组合补充

- content open 去重后单个等待者取消，不取消其他等待者。
- 保存 S10 后编辑 S12，关闭仍要审阅；Discard(S10) 也不能丢 S12。
- 布局中第 N 个 factory 失败，所有原窗口可见性/数量/绑定保持；提交后通知失败不假回滚。
- layout/recovery 请求中的旧 ViewId、新 generation 不发生误绑定。
- 新产品中运行 Inspector、编译与固定对象重链接、Save As 历史保留、Export Copy 不改 checkpoint。
- 视图关闭后已准入保存完成；扩展更新后旧结果仍安全释放。
- 旧 SDK/包目录不参与新安装消费者；删除旧导出后新插件和原 runtime 插件各自仍可用。

### 12.3 允许范围

Windows Editor/CPU/PLAYER、当前适用安装消费者、实际新双视口/验证层、原生输入、真实文件 IO、依赖/概念/operation负例和必要生成重建。

Linux/系统 IME/ASan 没有真实执行就保持 NOT_RUN。不要把此处改成新的环境建设项目，也不要因未测而宣称整个引擎跨平台失败或已通过。

原慢算法性能样本不补跑；仅改到的性能路径做有决策价值的短回归。测试必须使用隔离目录，不用隐蔽自动重试掩盖 Access denied。

## 13. 最终提交前的十问

1. 实际运行的 lux_editor 是否只进入新 EditorApplication？
2. 任一正式层是否仍通过公开头、私有 include、模板或传递链接回到旧产品？
3. P11 的每一条 P12 暂留项是否已经关闭？
4. 旧根是否真实不在 Git 树，是否被改名藏到新的子目录？
5. 正式运行/导入等仍有消费者的算法是否被误删？
6. SDK 的旧包/头/DLL/生成脚本是否仍能让旧 C++ 体系运行？
7. 所有用户功能是否有实际正式入口，而非仅通过模型测试调用？
8. 退出是否仍可靠结清，并保留文件和运行的真实结果？
9. C01/C03/C04 的原意是否由当前正式代码通过、历史原结果是否未被改写？
10. 所有未验证环境是否仍准确列出，main 和用户修改是否保全？

全部得到可复核答案才能报告 `PASS（明确的 Windows/当前范围）`。P12 若仍有旧 Context/Editor 活动消费者，不允许用“架构基本完成，P13再删”交付。
