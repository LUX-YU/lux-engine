# P07 R1 复审结论与 P08 启动指令

**日期：2026-09-30**  
**项目：LUX-YU/lux-engine**  
**实施分支：`codex/editor-redesign-v4`**  
**本次核对的验收 HEAD：`833d7efb18f8349cda9968ac3e4ea24ff2b54c60`**  
**R1 实现：`85ff8e23902db99b41c5df0b61d2b89985c198f1`**  
**R1 前置验收：`f6598e65f9d77620f1374065a76d82e6e6e00d79`**

## 0. 结论、证据边界与文档地位

**P07 R1 在本次源码与归档证据复审范围内通过，允许进入 P08。只执行 P08；完成后停下复审，不自动进入 P09，不修改 main。**

本次核对了远端分支、验收父链、R1 实现差异、两类 operation 的特殊成员修改、真实所有权测试、安装消费者的编译负例、修复前 Material/Flow 日志、修复后 SDK traits、一个实际 deleted-member 编译日志、完整 CTest 汇总以及归档检查器。随后读取原 P08 全文，并补读当前 Pane 头/实现和 Element 头以核对下一阶段起点。[S1–S13]

**没有独立构建或运行 Lux 引擎、133 项 CTest、11 项 PLAYER、54 项安装测试、八项编译负例或 GPU；没有运行完整 `check_receipt.py`，也没有逐份复算远端证据哈希。本次没有新增独立运行成绩。** 文中运行数字是实施方的归档成绩；测试源码阅读和日志抽查不等于第二套环境认证。

第 1–2 节是复审和已知交接事实。第 3 节以后是原 V4 `P08_interaction_and_detached_ui.md` 的实施展开，不是声称 P08 已实现，也不是对未阅读模块另作全仓审计。明确标为“建议”的内容是本次实施建议，其余对应原 P08 或已验收契约。原 V4 在启动包 `reference/v4/` 中逐字保留；不拿历史路径、种子账本或脚本覆盖现行工程。

## 1. P07 R1 为什么可以结束补正

### 1.1 类型准确禁止复制公共控制责任

`MaterialCompileOperation` 与 `FlowCompileOperation` 均显式删除复制构造、复制赋值、移动构造和移动赋值。不仅禁止 lvalue 复制，也堵住 rvalue 退化为复制的路径。[S2]

Material 通过工厂返回的 `unique_ptr` 转移唯一公共拥有权；Flow 由服务中的 `unique_ptr` 持有，外部使用 ID 或 const 借用。公开借用截止于相应记录确认或服务析构。编译结果依旧允许复制拥有型值或 shared_ptr；它们不是控制 owner。

内部 `shared_ptr<Impl>` 继续保有异步完成状态。两个生产 CPP 除“last public owner”改为“unique public owner”的注释外未改算法，原 cancel／Task 清理也保留。没有引入 use_count 取消协议，没有让执行器替误复制的公共 owner 兜底。[S2、S3、S5]

### 1.2 修复前证据来自实际 SDK，不是上一轮声明探针

读取到的 Material 修复前日志为：worker 已完成、业务完成尚未派发、原 owner 仍在，但复制副本析构后 `delivered=0 ready=0`。Flow 同条件还出现 `acknowledge_busy=1 capacity_stuck=1`。两者均记录 `author_unchanged=1`，因此不应误写成作者源损坏。[S6、S7]

R1 README 另记录不复制的对照为 `delivered=1 ready=1`，before 代码和安装头被冻结。非法复制程序在新 SDK 下不应继续作为正向运行测试；新接口必须在编译阶段拒绝它。[S3]

### 1.3 正向测试证明正确替代用法仍然可用

| 用法 | 读到的真实测试断言 |
|---|---|
| Material unique_ptr 转移 | worker 完成后、业务派发前，移动构造与移动赋值外层 unique_ptr；id/task/key/observed 保持 |
| 独立清理 | 另一个 operation 析构不清除当前 owner 的完成；完成交付一次，再派发为零 |
| Material 结果寿命 | 公共 owner 和一份结果指针释放后，另一份共享结果及值副本仍可读取字节与来源 |
| Flow 借用 | 显式 `const auto&`，静态断言借用类型；未派发时 acknowledge 为 BUSY、容量准确拒绝 |
| Flow 重试与容量 | 确认另一条记录后原借用仍有效；真实链接重试复用原 object；容量恢复可准入新记录 |
| Flow 结果寿命 | acknowledge 原记录后拥有型结果继续可读；另一操作不受影响 |
| 作者不变 | 两模型的完整编码、current、observed 和 dirty 保持 |

测试等待 `TaskInfo.finished`，不是仅 sleep 一段时间推测工作已经完成。没有为了通过测试，重跑 encoder 或放开原业务完成边界。[S4]

### 1.4 编译负例是预期契约拒绝

八种 case 是两类型各四个特殊成员。安装消费者以真实安装头/库配置，通过 OBJECT 负例验证 deleted member。脚本要求具体删除诊断，排除缺失头等无关失败；负例 target 的 `EXCLUDE_FROM_ALL` 用于按需运行预期编译失败测试，不是隐藏生产残留。[S2]

抽读 `reject_material_move_construct.log`，实际错误指向已安装 `MaterialCompilation.hpp` 的 deleted move constructor（MSVC C2280）。安装 traits 记录两类型四项均为 false。正常 `unique_ptr` 移动、编译产物复制由正向 static_assert 保留。[S8、S9]

### 1.5 验收和范围

归档完整 CTest 为 **133/133**。README 记录 **PLAYER 11/11、11 组 SDK 54/54**：原 44 项加两项真实所有权场景及八项编译负例。归档检查器核对旧 131 项名称、原 C++ 测试体、两个编译 CPP 的算法不变、前后安装头及拒绝原因；本次没有独立运行该检查器。[S3、S5、S10]

没有新增业务库、公共控制类型或过渡桥。没有发现必须继续阻止 P08 的 R1 范围问题。该判断不等于 P07 全部可能路径、全仓库或任意插件生命周期已经无缺陷。

## 2. 不得改写的交接事实

从已验收提交所在分支继续，不重置到最初 `2bb33ff1…`。先检查实际 HEAD、祖先关系和工作区；后来用户工作必须保留。验收 HEAD 的直接父提交确为上述实现 SHA。[S1]

沿用 `.internal/editor-redesign/` 作为唯一可变施工材料；P08 阶段末冻结到 `dev_log/P08/`。原 P07/P07-R1 和更早快照按各自 implementation_sha 验证，不改写原失败或哈希。原规范中 `docs/editor-redesign/receipts/` 是历史路径，不再建立第二套可变账本。

R1 报告明确保留 `editor/project/src/ProjectBuilder.cpp` 的用户修改，其 SHA256 为 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`，未计入资格实现。本次未访问用户本机；下一位实施者核对实际差异，不 reset。[S3]

C01 仍归 P09/P12，C03 归 P11，C04 归 P12；原 FAIL 和判定保持。P08 新问题独立登记，不能挂到这些编号延期。若底层 UI 变化确实影响原复现路径，保留原基准证据并解释新实测结果，不为维持 FAIL 而人为引入缺陷，也不删断言改变原契约。

P06 的 Physics2D 生成依赖窄修复与原冷构建失败记录保持。本轮没有新增全仓首次冷构建资格。完整新产品双视口像素、布局与 GPU 退休资格仍按 P10/P13，P08 不能拿纯模型或 Fake 测试替代它们。[S3]

## 3. P08 目标：交互与作者内容分开，构造与挂载分开

P08 有两个互相关联但不应混成一个 Manager 的工作面：[P08]

1. **交互域**：选择和手势维护临时意图，最终调用三类既有作者模型；不拥有 Session，不另建历史或保存基线。
2. **UI 基础与 view_api**：先离树构造完整对象，再在安全点准备/接管/挂载；关闭时先撤销输入与注册，再在允许的生命周期边界析构。

P08 不实施 P09 布局存储、P10 完整桌面 ViewHost 或新编辑器产品、P11 完整插件命令注册，以及 P12 总入口切换。提供可测试的 FakeViewHost 与真实底层挂载能力即可，但不能只交付 Fake、空接口或转发旧构造器。

```text
SessionStore ──唯一拥有──> SceneSession / MaterialSession / FlowSession
                              ↑ 短时读取、一次已提交编辑
具体 Interaction ──拥有──> 选择、起始身份/戳、临时手势覆盖
        ↑ 视图关联         （不强拥有 Session、不保存可写 Registry）
        │
未来 ViewHost ──唯一拥有──> Pane ──明确拥有──> Element / 容器子节点
        │                     │
        └──────── Root 的活动注册、路由、焦点与父子观察关系
                      （关联不是另一份 delete 责任）
```

相机/hover/输入捕获属于视图本地，不因共享选择组而共享。P06 运行域与作者域仍分开：运行中的临时调试只改运行实例，不进入作者撤销栈。

## 4. 当前源码起点与必须先查明的部分

本次读取的 `Pane.hpp` 只有 rooted 构造入口。`Pane.cpp` 在构造中调用 `checkContentChange()`、`attachTo(parent)` 和 `registerPane()`；析构调用 `root().unregisterPane()`，`root()` 在未绑定时触发契约失败。`setContent`、标题和 modal 更新也直接使用 Root。[S11、S12]

`Element.hpp` 的构造依赖 Pane/Element，公开 `root()`、`pane()` 返回引用；私有成员包含注册 slot 和 parent/pane 指针。因此新增离树状态要覆盖这些调用的语义，不能只增加一个名称为 createDetached 的工厂。[S13]

**本次没有重新穷举 LuxObject 的父子析构、Dispatcher、Root 路由和所有控件实现。** P08-A 必须实读这些实现并写所有权/调用表，不能从字段名推断父节点会不会 delete 子节点。

原 P08 的 `Pane(PaneId, PaneTypeId, title)`／`Element(ElementId)` 是离树签名方向。若真实 LuxObject 构造必须绑定 owner Dispatcher，应保留或显式传入受限的 dispatcher/线程亲和端点；这属于签名落地，不是重新传入整个 Root、EditorContext 或建立全局默认 Dispatcher。构造不注册 Root，不等于对象没有线程归属。

## 5. 类型、关系与目录：按角色落地，不再扩散主题

下表继承原 P08。新名字是目标角色，不冒充已经存在的实现；先核对既有等价类型，能复用就不重造。

| 类型/角色 | 组合或继承 | 职责与生命周期 | 归属 |
|---|---|---|---|
| SceneInteractionGroup / SceneSelection / InteractionGroupId | 具体交互 owner 组合状态 | 选择组、起始戳、活动手势；不强持 Session | `editor/tools/scene/interaction/` |
| 作者对象引用 / RunningObjectRef | 区分来源的封闭值组合 | 复用 P02、P06 已有引用与代际；原表 EditedObjectRef 先映射实际符号 | 同主题 InteractionTypes |
| MaterialInteraction / FlowInteraction | 各自具体组合，不继承万能图文档 | 临时拖拽、候选连接和选择；提交调用各自领域编辑 | 各工具 `interaction/` |
| Gesture | 私有互斥状态，建议由具体 interaction 直接拥有 | 起始目标/戳、临时值、提交或取消；不是另一份 History | 各具体交互实现 |
| ViewId | Host 域、slot、generation 值 | 当前运行期的视图寻址；旧排队请求不得击中新对象 | `editor/views/api/` |
| ViewRestoreKey | 独立 tag 的稳定键 | 跨启动恢复匹配；不是当前活动句柄 | 同一 view_api |
| ViewTypeId | 直接复用 PaneTypeId 的类型身份 | 此 alias 是明确相同概念，不是保留旧逻辑的兼容壳 | 同一 view_api |
| PaneId | 原 UI 树注册名 | 每次挂载由 Host 按现有协议分配；不与 ViewId 强转 | 原底层 UI |
| ViewInfo | 拥有型只读描述 | 不暴露 owning vector、Pane 指针列表或 Session 实现 | 同一 view_api |
| DetachedView | CodeLease + unique_ptr<Pane> 的组合 | 唯一未挂载候选 owner；只允许转移责任，不复制 | 同一 view_api |
| IViewHost / ViewRequests | 窄宿主契约 / 请求值 | close/show/focus 使用 ViewId；不暴露 erase/adopt 权限给普通业务 | 同一 view_api |
| Pane / Element | 保留真实 LuxObject/UI 继承 | 离树对象、内容与子节点拥有关系准确；Root 只观察注册 | 原 `modules/function/ui/` |
| PreparedMountBatch / PreparedDetachBatch / AttachmentState | 原 Root 的私有准备/提交状态 | 一次准备、一次消费或取消，不复制挂载/卸载责任 | 原 UI 私有实现 |

保留 `editor/editing/history/`、`editor/editing/sessions/`。Material/Flow/Scene 的交互在各自工具主题内，不新增顶层 GestureManager、SelectionRuntime 或每种小类型独立库。必要的 interaction 与 view_api 是依赖边界，不是要求同样数量 DLL。

## 6. P08 内部实施顺序

### P08-A：先盘点真实拥有关系与生命周期入口

逐对象记录 Pane、Element、值成员子控件、unique_ptr 容器、LuxObject 父子关系、Root 活动 slot、路由、焦点和 capture：谁构造、谁删除、何时登记、何时撤销、哪些回调会再次请求结构修改。

同时核对旧 EditingGuard、editing_busy/busy/finishing、字段手势、Scene 选择、Material/Flow 节点位置、画布导航。区分重复门控与真正不同的交互状态，不能见 bool 就删。以当前符号/AST 与迁移账本为准。

将新类型的 copy/move、失败后 owner、候选清理、旧兼容消费者先写成契约与小测试，再改大调用面。不先创建十几个空 Manager 等待以后接线。

### P08-B：实现三类具体交互协议

Begin 捕获有效来源与起始 ContentStamp；Preview 只更新临时覆盖，保持作者编码、current、dirty、checkpoint 不变；Commit 将最终值形成一次领域批次；Cancel 仅丢弃临时状态。[P08]

**建议默认采用严格起始戳冲突拒绝。** 已有具体字段合并策略可复用并验证，但本阶段不新建通用 rebase 系统。冲突时不得偷偷读取新内容作为旧手势的新基线再覆盖别人的修改。

拖动结束后恰一次历史；取消和净无变化不制造伪历史。不得通过每帧提交再合并 Undo 来冒充临时预览。普通保存/冻结编译在拖动期间应看到已提交内容，不能借机强制完成手势。

Material/Flow 已保存的节点布局仍进入作者编辑；临时连线、拖拽轨迹及画布 pan/zoom 不是同一种状态。保留原功能，不把持久布局全部移成易失 UI 状态。

作者删除、重载、关闭或运行停止时验证引用并撤销无效选择/手势。不能跨帧保留可写 Registry 或编辑 scope；共享 group 只共享约定的选择与手势，不共享各视口相机。

### P08-C：先做真正离树的底层 UI

在 owner 线程构造 Pane、Element 及其子对象，不注册到任何活动 Root，不占焦点/capture，不改变 Session。新析构路径允许从未挂载的完整或部分构造对象清理。

新增 `attachedRoot()` 的空观察语义；路由/焦点请求返回定义明确的 NotAttached 或现有等价错误。离树时允许本地属性修改的范围写清，不能经 `root()` 解引用空指针，也不能把所有调用一概静默成功。

若 Pane 以值成员拥有子 Element，值成员已经是 owner；禁止将其地址再包装 unique_ptr。原 parent 对象若有 delete 行为，需要在 UI 适配层消除双责；不得全局修改 LuxObject 通用语义来迎合一个工具。

旧 rooted 构造只作为已登记旧产品路径暂留到 P12，新工厂不能调用它。兼容路径尽量复用同一底层注册机制，不能形成两套活动树算法。

### P08-D：实现准备—提交—通知及卸载

准备阶段完成 Host 拥有容量、Root slot、路由、挂载关联及必要通知容量。失败时旧 UI、焦点、路由与作者状态不变，候选的 code/data 寿命正常释放。[P08]

安全点提交只转移已准备的 owning 指针和关联，不调用任意 provider 的 create/restore，也不把可恢复的分配失败移到提交之后。准备与提交间若允许其他结构变化，必须验证准备令牌仍有效；不能用失效槽位继续 commit。

通知在提交之后发布。通知期间再次请求挂载/卸载，应进入下一批，不能递归改正在遍历的集合。通知失败保留“已挂载”事实，单独报告诊断，不能谎报整个创建从未发生。

卸载先停止新路由、解除焦点/capture/观察关系，再注销，待活跃调用栈退出后析构或移交资源退休责任。继续使用已有 deferChange 与生命周期检查，不通过 shared_ptr 无限保活节点替代安全点。

P06/P07 的重资源退休协议保持：UI 失效不意味着 GPU 已完成。P08 提供交接与验证，不重新设计 RenderRuntime 或再添一份退休 manager。

### P08-E：窄 view_api 与真实协议测试

提供 ViewId、ViewRestoreKey、ViewTypeId、ViewInfo、DetachedView 和请求接口，使用 FakeViewHost 验证队列及身份约束，同时使用真实 Root/Pane/Element 验证注册与析构。Fake 不能代替底层实际挂载。

view_api 可以依赖必要 UI 协议与 contracts，不依赖具体 SceneSession、ProjectStorage、RenderRuntime、CommandRegistry。三个 interaction 只拿具体领域的受限能力，新代码不重新拿整个 EditorContext。

完整桌面 ViewHost 留给 P10，不因要测试挂载而先重写应用入口。运行期 ViewId 与恢复键必须从这一阶段起分开，排队请求不能只持裸 Pane 指针或稳定名称。

### P08-F：删除到期新路径副本，迁移消费者并验收

迁出的纯交互规则只保留一个实现；新路径不再靠多处 EditingGuard/重复 busy 协调同一编辑门。旧 UI 尚未切换的适配保持限定消费者/P12 期限，不能把保留旧入口误报为完整迁移。

当前 Pane/Element 的 rooted 构造和旧 `root()` 不是 P08 全仓必须删除项；原 P08 明确允许旧产品使用到 P12。删除约束精确限定新路径与迁出的算法体，避免为了清理字符串造成旧产品停摆。

更新构建/安装、生成输入、实际私有 include、测试与账本。模块公共头变化时按已有安装前缀同步要求处理；没有实际 Android 构建就不报告 Android 通过。最后绑定实际实现 SHA，显式 P08 门禁。

## 7. 将本轮所有权修正直接用于 P08，但不要机械禁止所有移动

以下是对原 P08 所有权要求的具体实施建议，不是追加全仓特殊成员审计：

| 类别 | 建议的特殊成员策略 | 必须验证 |
|---|---|---|
| 活动 Pane/Element、固定地址的宿主/交互 owner | 按真实基类约束不可复制，通常不按值移动 | 不因为拥有指针可复制就让活动注册责任复制 |
| DetachedView／可转移未提交准备责任 | move-only；操作必须转移唯一责任 | moved-from 析构无副作用，重复消费被拒绝，失败只释放一次 |
| ViewId／类型 ID／恢复键／配置值 | 可复制值 | 复制不产生关闭或注销责任，不允许跨域强转 |
| ViewInfo／完成诊断／只读快照 | 拥有型、可按声明复制 | 结果生命周期与重资源分开；动态 payload 的 code/data 保活 |

**成员声明顺序只解决析构顺序，不自动解决移动赋值。** DetachedView 先 code 后 Pane 可以保证普通析构时 Pane 先结束，但默认移动赋值可能先替换旧 code，再销毁旧 Pane。实现 move-only holder 时必须逐项论证替换路径；可复用已有正确拥有单元，不为它另造公共框架。

同理，PreparedMountBatch/DetachBatch 有一次性责任，不能仅因为使用 shared_ptr 便允许复制公共提交权。也不能通过保存原输入的裸引用跨过 provider 回调或 owner wait。

保留 P05 R2 原则：新业务请求可按状态/容量拒绝，已经接受工作的完成事实必须可靠接收。只记录完成的内部入口不能因外层保护丢结果，也不能解除外层的防重复/防删除保护。

## 8. 六项原 X08 与必须记录的观察

| 原编号 | 本阶段验证内容 | 不足以通过的替代物 |
|---|---|---|
| X08-01 | 三模型真实手势：Preview 时 capture/encode/保存捕获只含已提交值；Commit 一条 History；Cancel 零变化；冲突不覆盖 | 只测临时结构或每帧 edit 后再清历史 |
| X08-02 | 作者与 Run 来源区分、同数值实体与槽位复用、重载/关闭后拒绝旧引用 | 仅比较裸 Entity 或复制 Session 身份 |
| X08-03 | 真实离树成功、构造/子节点失败；Root 活动计数、焦点与注册不变、唯一析构 | 工厂返回 unique_ptr，但内部仍 rooted 注册 |
| X08-04 | Host/Root/通知准备容量失败；旧 UI 不变，候选/代码顺序正确 | 缺依赖引发构建失败，或删断言当作故障验证 |
| X08-05 | active draw/signal 自关闭；回调返回前不析构，新路由停止，下一安全点释放一次 | 立刻 delete，或永久 shared_ptr 持有不再回收 |
| X08-06 | 离树的 root/focus/route 明确无 Root/NotAttached；允许本地配置；挂载后正常 | 空指针访问、默认成功或整个控件禁用 |

X08 的深化场景应归入上述原编号，不另造平行阶段：DetachedView 移动赋值释放旧候选；回调内排队结构操作；post-commit 通知失败；失效 mount 令牌；关闭最后视图不擅自销毁作者会话。具体处理需与本轮实际 API 对应，不能只有“测试名称列表”。

原 133 项 Editor、11 项 PLAYER 和 11 组 SDK 基线继续保留其行为，不以数量代替断言。P08 改动 UI API 后允许必要迁移测试，但逐项解释等价或更严格的契约；原 P07 R1 八项安装编译负例继续拒绝，两个正确所有权场景继续通过。

构造/挂载故障测试使用真实底层 UI；已有 GPU/旧产品回归不能因改构造方式被关闭。P08 的协议测试不是完整 P10/P13 新产品像素或 IME 资格，报告分开说明。

依赖负例至少保护：Engine UI 不依赖 Editor；view_api 不依赖具体模型/存储/渲染服务；作者模型不反向依赖 interaction；新 interaction 不借旧 Context/Impl；新工厂不走 rooted 兼容入口。每个失败命中预期规则，同一夹具修复后通过。

## 9. 删除、暂留与交付清单

| 项目 | P08 处置 |
|---|---|
| 新路径中的 EditingGuard 和重复同义门控 | 使用唯一 Session gate 加具体 Gesture 状态，迁出后删除旧算法体 |
| 新构造中注册 Root 的副作用 | 删除；活动登记统一发生于准备/提交路径 |
| 新代码对 rooted ctor、无条件 `root()` 的依赖 | 迁移为离树及显式 attached 查询；不能新增同义旁路 |
| 旧 rooted 构造与旧产品必需调用 | 限定消费者、期限 P12；不扩大到新工厂 |
| Root/Pane/Element 原生命周期与 deferChange 检查 | 保留并适配，不为通过测试关闭检查 |
| Scene/Material/Flow Impl 的跨模块 friend | 新交互/UI 不得新增这种权限；只允许确切 Root/Host 协作 |
| 旧格式数据、原失败日志、before 非法代码 | 独立保留对象；不作“垃圾代码”删除 |

删除包括声明、定义、原调用、活动 friend、生成输入、CMake/安装导出和示例；换名备份、`#if 0`、空转发不算删除。预期编译失败测试的按需 target 与生产残留区分。

最终报告必须包含：三交互与 UI 树的 owner 表；对象身份域；挂载/卸载提交点；通知失败语义；特殊成员；已删/暂留的准确符号及消费者；六项 X08、相关 Q、依赖负例、PLAYER、SDK 和实际 UI/GPU 范围。

实现和验收独立提交。门禁显式 `LUX_EDITOR_MIGRATION_STAGE=P08`。必测未跑、环境缺失或到期项未迁完，状态只能 PARTIAL/BLOCKED；不能提前 P09，也不能通过把 stage 留在 P07 得到假 PASS。

## 10. 可直接交给实施方

> P07 R1 复审通过，允许进入 P08，仅执行 P08。以 `833d7efb18f8349cda9968ac3e4ea24ff2b54c60` 为已验收前置；检查实际分支/祖先/工作区，保护 ProjectBuilder.cpp，不 reset、不修改 main。
>
> 读取原 P08、本启动补充及当前施工账本。按 A–F 内部顺序实现三类独立交互、真实底层离树构造/挂载/卸载和窄 view_api，不实施 P10 完整桌面宿主或 P12 产品切换。
>
> Begin/Preview 不改作者编码、current、dirty、checkpoint；Commit 经具体域一次提交；Cancel 丢弃临时覆盖。复用作者/运行身份和唯一 gate，不保存跨帧可写借用或平行 History；保留图的持久节点布局。
>
> 离树构造不得注册 Root；先准备容量和路由、安全点提交、之后通知。卸载先撤销输入/焦点/关联，等待回调退出及资源责任交接后析构。Root 非第二 owner，不改变 LuxObject 通用语义；必要 dispatcher/线程亲和按真实实现保留。
>
> 新 owner 明确 copy/move 契约。DetachedView 和一次性准备责任不可复制，转移及移动赋值保证旧节点在其 code 释放前清理；结果值和身份仍按真实值语义复制。不撤销 P07 R1 的 operation 约束，不改执行器算法。
>
> 保持 editing/history、editing/sessions 及各工具 interaction 的主题目录。旧 rooted 构造/旧产品壳限原消费者且最迟 P12 删除，新工厂不得使用；迁出算法不保留第二份。
>
> 执行 X08-01～06、相关 Q、原 133 项行为、PLAYER、SDK、八项 operation 编译负例、真实 UI 生命周期/依赖负例及原 GPU 回归；显式 P08。不要把 FakeViewHost 或纯 CPU 状态检查写成 P10/P13 新产品验收。
>
> 原历史证据不改写；`.internal/editor-redesign/` 维护唯一施工材料，阶段末冻结 `dev_log/P08/`。C01/C03/C04 保留原契约与后续责任。分别提交实现和验收、正常推送，停在 P08 等待复审，不自动进入 P09。

## 11. 固定来源

以下链接固定到本次验收提交，避免分支更新改变证据。S3 中完整运行矩阵为实施方报告；S6–S10 只抽读了列明内容，未独立重跑。

- [S1 验收提交及父链](https://github.com/LUX-YU/lux-engine/commit/833d7efb18f8349cda9968ac3e4ea24ff2b54c60)
- [S2 R1 实现差异](https://github.com/LUX-YU/lux-engine/commit/85ff8e23902db99b41c5df0b61d2b89985c198f1)
- [S3 R1 README](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/dev_log/P07-R1/README.md)
- [S4 真实所有权测试](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/editor/tools/material/preview/test/ownership.cpp)
- [S5 归档检查器](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/dev_log/P07-R1/check_receipt.py)
- [S6 Material 修复前](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/dev_log/P07-R1/before/material-copy.log)
- [S7 Flow 修复前](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/dev_log/P07-R1/before/flow-copy.log)
- [S8 安装 traits](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/dev_log/P07-R1/logs/after-sdk-traits.log)
- [S9 安装移动构造拒绝诊断](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/dev_log/P07-R1/logs/reject_material_move_construct.log)
- [S10 CTest 汇总](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/dev_log/P07-R1/logs/ctest.log)
- [S11 当前 Pane 声明](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/modules/function/ui/include/lux/engine/ui/Pane.hpp)
- [S12 当前 Pane 构造及析构](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/modules/function/ui/src/Pane.cpp)
- [S13 当前 Element 声明](https://github.com/LUX-YU/lux-engine/blob/833d7efb18f8349cda9968ac3e4ea24ff2b54c60/modules/function/ui/include/lux/engine/ui/Element.hpp)
- [P08 原 V4](reference/v4/phases/P08_interaction_and_detached_ui.md)；完整原包保留配套相对路径，仅供历史参考。
