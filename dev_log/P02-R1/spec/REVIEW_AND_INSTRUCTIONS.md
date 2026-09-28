# LUX Engine：P02 复审、定向补正与目录收拢指令

日期：2026-09-28  
实施分支：`codex/editor-redesign-v4`  
审阅 HEAD：`3a4d6ce367cfe3b9551ccde01a0de6d7088fa256`  
P02 实现：`fb55a5a9694831242e537a5cb1b3f7080a6f25fe`  
前置 P01 R1：`3bc49e44646df30ea73990864658a9121789757a`

## 0. 结论和证据边界

P02 已实现实质性的独立 Scene 作者模型。历史、保存基线与作者数据的分离应保留，不重做。源码和归档测试显示，已能在 CPU 模型上进行真实编辑、撤销、冻结及增量查询。

**本轮不直接进入 P03。先完成 R1-A 混合批次补正、R1-B 编码读取重入保护。再以独立提交完成 S01 目录收拢，最后重新验收并停下。**

R1-A 是已读控制流中的旧临时值覆盖新组件问题；R1-B 是插件编码回调重入时快照内容戳可能与正文不一致的条件性问题。两者都需要实施方先增加真实负例、保留修复前结果。本轮没有完整本地源码和依赖构建环境，未独立运行引擎、GPU 或这两个新增负例。不得把源码推导写成已经观测到的运行崩溃。

目录收拢来自用户的新要求，也是对 V4 物理布局的修订；它不等于 P02 的功能实现失败。未提交排版差异也不自动推翻固定 SHA 上的验收，但必须在下一轮纳入清晰交接。

源证据编号见文末。原 P02 README 的 PASS 是实施方报告；本文件记录的是独立复审意见，两者不得互相覆盖或伪造为同一次验收。

## 1. 应保留的成果

| 已读内容 | 本轮判断 | 不应因此扩大的工作 |
|---|---|---|
| SceneSession 组合 SessionState、SceneSource 和唯一 EditHistory | 概念方向正确；currentContent 从实际历史读取，没有缓存第二份 current | 不重写历史，不加入第二套 SessionStore |
| SceneSource 内部私有 CPU Registry、WorldObjectId、未知 payload | CPU 数据容器不等同于运行实例；使用 ECS 本身不构成违约 | 不为了“无 Runtime”再发明一套世界格式 |
| 结构准备完整候选后交换、字段局部解码 | 已有明确的准备/提交边界；整源结构成本已披露 | 不把大世界性能优化加入本轮必测 |
| snapshot 编码组件并复制包/卷字节、克隆正式描述 | 比外层 const shared_ptr 更有实质隔离 | 保留未知数据及代码寿命测试 |
| changesSince 有界记录和 RESET_REQUIRED | 正确方向，应保留历史换代与裁剪语义 | 不引入全局 EventBus 或无限日志 |
| 43/43 CTest 归档、六组七项消费者报告 | 证据不是仅有口头计数；本轮未独立重跑全套 | 不用原成绩替代修改后的新测试 |

已披露限制：有索引 World 的内容编辑和分区/索引/存储结构修改会拒绝；结构编辑准备完整 CPU 候选；预算不是整个进程 RSS 硬上限。这些限制不得隐瞒，但不应在本轮强行扩大为新功能项目。[S1][S2][S3][S6][S7]

## 2. R1-A：混合批次里的字段临时值跨越组件删除/重建

### 2.1 精确位置

- `editor/tools/scene/model/src/edits/SceneObjectEdit.cpp`
- `prepareInput()`：`result.fields`、`scratch`、`SceneRemoveComponent`、`SceneAddComponent` 分支，以及末尾遍历 `result.fields` 回写 `after` 的循环。
- 公共输入：`SceneEditBatch::edits`、`SceneSetField`、`SceneRemoveComponent`、`SceneAddComponent`，定义于 `model/include/lux/engine/editor/scene/SceneEdit.hpp`。[S4][S5][S8]

### 2.2 现有路径

第一次对某个对象组件执行 SceneSetField 时，代码从当时的组件编码建立 scratch，并在 result.fields 中登记 before。后面的字段修改按 object/schema 复用该 scratch。

SceneRemoveComponent 和 SceneAddComponent 只修改 after 对象表中的组件集合，不使已有字段 scratch 或字段记录失效。批次最后统一编码 result.fields，并以整个 SceneComponentData 覆盖 after 里的同 schema 组件。

因此有如下输入路径：

```text
原 Transform3D：translation = (0,0,0)，scale = (1,1,1)
1. SetField translation = (1,2,3)  -> scratch 保存旧组件 + 新 translation
2. RemoveComponent Transform3D    -> after 移除该组件
3. AddComponent Transform3D       -> after 加入 translation=(8,9,10)、scale=(2,3,4)
4. 批次收尾回写字段 scratch        -> 整个新组件被旧 scratch 覆盖
```

按现有控制流，后加入的 scale 会退回旧值。若规定列表顺序，第三步的新 translation 也应保留；即使采用“最后在新组件上应用字段”的声明式语义，未被修改的 scale 也不应无说明丢失。

**问题不是缺少一条 if，而是临时编辑状态没有区分组件存在周期。** 原子提交只能保证一次写入，不能保证这次写入准确表达了所有已接受意图。

现有测试覆盖字段与重挂父混合、单独增删组件及失败回滚；未在已读测试中发现该删除/重加交错组合。[S9]

### 2.3 本轮采用的语义决定

1. `SceneEditBatch::edits` 是有序意图列表；预先规划 UUID 可用于前向身份引用，但不能偷偷改写同一组件的更新顺序。
2. 同一对象组件的移除结束该组件的当前临时编辑状态；后续添加以新载荷作为基线。
3. 后续字段修改应用到此时存在的组件。字段操作定位到不存在的组件时，整个批次失败，不返回成功或忽略。
4. 成功批次只产生一个 History 提交；Undo 恢复批次前整体内容，Redo 恢复批次后整体内容。
5. 对当前不支持的交错组合，可以在写入前明确拒绝并完整保留现场，但不能将“拒绝所有混合批次”作为本轮完成。原先已支持的混合用例必须保留。本轮上面的删除/重加用例应支持。

这是对原批次契约的具体化，需记入当前施工规范；原指导没有逐项规定所有交错，不应伪造为过去已经明确规定。

### 2.4 修改方式

优先保持已有结构候选与字段局部路径，不另造 SceneEditEngine 或第二套撤销栈。

- 将同一对象/schema 的字段 scratch 与当前 staged 组件关联。
- 在移除/添加分支处理该关联的结束与重建；不再让旧 scratch 在末尾无条件覆盖新载荷。
- 一种可行实现是：只为被触及的组件保留一个私有 staged component，所有字段/存在性变更更新它，最后从同一 staged 状态生成对象 delta。若沿用现有容器，也必须在组件生命周期变化时清理/重建 scratch 和字段记录。
- 纯字段批次仍只解码受影响组件；不能为修补交错而使每次字段编辑都重建整源。
- 不引入公共组件 generation 类型、全局同步表或新的框架 target；这属于 prepareInput 的私有准备状态。
- 清理被替代的回写路径、无效字段缓存及重复编码逻辑；不保留“新旧两种 batch 模式”开关。

### 2.5 验收

见附录测试 R02-01～R02-04。修复前应在固定实现 SHA 上记录真实失败；修复后测试要核对完整编码组件（尤其未直接编辑的 scale），不是只检查 apply 返回成功。

## 3. R1-B：编码读取只有准入检查，没有覆盖回调全程的稳定访问

### 3.1 精确位置

- `model/src/SceneSession.cpp`：`SceneSession::capture()`、`Impl::available()`。
- `model/src/SceneSource.cpp`：`SceneSourceAccess::capture()`、`objects()`、`component()`、`SceneReadView::component()`。
- `editor/sessions/include/lux/engine/editor/sessions/SessionState.hpp`：现有唯一 EditGate。

capture 先确认 AVAILABLE，随后在没有进入 gate 的情况下调用配置和组件 codec。组件路径会调用 schema->capture 以及 ComponentCapture::encode；这些是可由插件提供的回调。当前 apply 则只依赖同一个 gate 的 withEdit 准入。[S2][S3][S10]

### 3.2 无需线程的失败场景

使用当前 plugin_snapshot.cpp 的插件组件模式，在其编码回调中加入一次性测试钩子：

```text
外层 capture 开始：取得内容戳 S0
    插件组件编码回调：
        对另一对象的 Transform3D 调用 session.apply()
        当前 gate 仍 AVAILABLE -> 可进入编辑 -> 提交 S1
    外层继续编码另一对象，读到 S1 的 Transform3D
外层完成：快照字段已含 S1，但入参内容戳仍是 S0
```

使用不同组件的局部字段修改可避免递归编码同一插件组件。现有测试已经证明该字段路径不会解码无关插件节点，因此可用一次性回调建立小型真实负例。[S11]

这是条件性源码推导，不宣称普通内置 codec 必然会重入。不需要恶意线程或私有 Registry 写入：测试只调用公开的 session.apply。编码回调重入结构编辑还有源存储失效风险，但本轮首先使用稳定、可诊断的内容戳不一致负例，不以制造崩溃作为唯一证据。

### 3.3 修改契约

需要一个覆盖编码、错误返回、回调局部值析构直至退出的稳定读取作用域。

- 复用 SessionState 所拥有的唯一 EditGate；必要时增加语义准确的 `READING` 阶段和窄 `withRead` 操作。
- 不用 `withEdit` 冒充读取，也不在 SceneSession 增加第二个 `capture_busy_`。
- 读取期间通过公开业务入口重入 apply/undo/redo/reload adopt/prepareClose/prepareBindingChange，应明确拒绝或由更高层显式排队；领域层不暗中排队。
- 简单标量描述和 currentContent 可保持可读；禁止重新让关闭查询构造完整 describe。
- 回调失败或抛异常后，作用域必定恢复；返回值不能含已发生源变化的过期内容戳。
- `SceneReadView::component()` 同样经过 codec，不得遗漏。可以让 ReadView 私有地借用同一 gate，在每次拥有型组件读取时进入它；不得让 ReadView 获得关闭、持久化或全局 Context 权限。另一方案是将回调型读取收敛到 Session 的明确入口，迁移所有调用后删除旧入口；不要永久维持两条不同保护等级的 API。
- 普通 `objects/contains/parent` 等同步无回调借用无需自动升级成长期共享锁；保留“借用不跨修改”的现有契约。
- 不要只在返回前比较版本：那至多检测不一致，不能保护回调期间源引用的寿命。

若新增 ReadScope，放在现有 gate 语义头/实现中，保持内部或最窄可见性。它不是新增子系统，也不需要新库。

### 3.4 验收

见 R02-05～R02-08：编码回调重入、回调失败/异常后恢复、关闭/重载准入，以及 owning component read 的同类回调。禁止只检查“最后还没崩”而不比较内容、历史、观察版本和 gate 状态。

## 4. 对目录过度分散的判断

用户提出将 `editor/history` 收进 `editor/editing` 是合理的。V4 物理布局将太多逻辑边界直接落实为顶层同级目录和独立包，这一部分应收敛；实施方按原路径施工不构成误解。

必须分清四个层次：

| 层次 | 要回答的问题 | 本轮决定 |
|---|---|---|
| 类型/职责边界 | 谁拥有内容、历史、checkpoint、视图 | 保留已经论证的责任分离 |
| 源码目录 | 维护者到哪里寻找相近功能 | 将历史和会话归到 editing 主题目录 |
| 构建 target | 谁能看见/链接哪些依赖 | 先保留 edit_history、edit_sessions 的独立可检查边界 |
| 安装/动态库包装 | SDK 如何交付、是否必须一个模块一个 DLL | 不应与目录机械一一对应；本轮不同时改 ABI 和包名 |

CMake 可以在同一主题目录中维护多个 target，也可以由一个 target 收集多个子目录的源码。减少根目录数量不要求把所有实现链接成同一个库。[S14]

### 4.1 当前结构的具体问题

1. 顶层同时加入 contracts/history/sessions/editing，相关编辑能力的入口被分散。
2. 旧 editor_editing 当前仍编译 EditHistoryTarget.cpp 和私有 LegacyPersistenceState，并 PUBLIC 依赖 edit_sessions。直接让纯历史“并回这个 target”会混入兼容职责；若保留原依赖，还可能形成反向/循环链接。
3. 新 scene_model 位于 tools/scene/model，而旧 editing/scene 仍是依赖 Runtime/UI 的 Registry 适配。两者暂时并存有迁移原因，但名字容易被误当成两个长期 Scene 作者系统。
4. 当前公开 include 已有 `lux/engine/editor/editing/EditHistory.hpp`，说明物理 editor/history 并非稳定 API 必须保留的路径。[S12][S13]

### 4.2 S01：本轮仅做这个目录迁移

```text
editor/
  contracts/                      # 本轮不动，不为减少一行再改所有基础路径
  editing/
    CMakeLists.txt                 # 先添加 history/sessions；再构建暂留旧协议 target
    history/                      # 原 editor/history 整目录 git mv
      CMakeLists.txt
      include/...
      src/...
      pinclude/...                # 原有内容存在才移动，不创建空壳
    sessions/                     # 原 editor/sessions 整目录 git mv
      CMakeLists.txt
      include/...
      src/...
      test/...
    include/...                   # 旧领域编辑协议，按既有期限暂留
    src/EditHistoryTarget.cpp     # 旧协议的明确实现
    scene/...                     # 旧 Registry 适配，P12 删除/完成迁移
  tools/
    scene/model/...               # 不并回 Runtime/UI
  transition/
    LegacyPersistenceState.*      # 原唯一桥，P12 到期，不挪入新历史库
```

保留的 target 名称：`edit_history`、`edit_sessions`、当前暂留的 `editor_editing`。保留现有命名空间、include 路径、可见性宏、输出库名和 package 名。原 `editor/history`、`editor/sessions` 路径迁移完应不存在；不保留 forwarding CMake、转发头、符号链接或 `.old` 副本。

不把源文件直接丢回 `editor/editing/src/EditHistory.cpp`。该精确旧路径属于已到期 F001；新归属是 `editor/editing/history/src/EditHistory.cpp`，旧算法仍只有一份。

### 4.3 CMake 修改顺序

当前根 CMake 的关键顺序是 contracts → history → sessions → tools/scene/model → editing。迁移后可改为：

```cmake
add_subdirectory(contracts)
add_subdirectory(editing)            # 内部先定义 history 和 sessions，再定义旧 editor_editing
add_subdirectory(tools/scene/model)
# project/storage/assets/metadata/plugins 等沿实际依赖继续
# 原 add_subdirectory(editing/scene) 仍留在其依赖都已定义的位置
```

`editor/editing/CMakeLists.txt` 在旧 target 定义前加入：

```cmake
add_subdirectory(history)
add_subdirectory(sessions)
```

这里的 CMake 是方向明确的局部修改，不要求照搬整个根文件；以实际依赖和现有宏行为验证。不得出现重复 add_subdirectory，同一源不能被新旧 target 各编译一次。

### 4.4 必须同步修改的路径消费者

| 位置/类别 | 修改要求 |
|---|---|
| editor/CMakeLists.txt、editor/editing/CMakeLists.txt | 更新添加路径及定义顺序 |
| 移动后的 history/sessions CMake | 检查相对源码、脚本、生成目录；保留 target 接口 |
| cmake/installed-consumers/editor-sessions/CMakeLists.txt 及其他实际引用 | 修正指向原 test/sessions.cpp 的相对路径；消费者不能因此获得源码 private include |
| editor/tests/architecture/rules.json | 更新两个目标的 path、new_scopes、foundation/source include 归属；保留同一允许/禁止边 |
| 架构检查器及负例夹具 | 更新真实 fixture 目录；新 scope 不能扩大为整个 editor/editing，把旧协议误当纯基础 |
| 历史纯化检查 | 将只查 editor/history/ 的限制迁到新精确路径，不能因移动使检查悄悄失效 |
| scope_compile.py 等测试脚本 | 核对 parent 层数和相对源码路径，不仅替换 CMake 字符串 |
| 当前 migration-ledger/architecture/test-coverage | 记录 MOVE_FROM/MOVE_TO，保持责任与 P12 期限 |
| 当前实施规范 | 加入显式路径修订表，后续 LLM 遇到旧路径沿表追踪，不重建旧目录 |
| 历史验收脚本 | 旧源码存在性/哈希检查针对其 implementation_sha 的 Git tree 或当时归档；当前审计针对新路径 |
| 编辑器文档和示例 | 更新活动指引；冻结历史证据里的旧路径作为历史事实保留 |

不得通过修改 P00/P01/P02 原收据的 implementation_sha、路径和测试成绩，假装迁移早已发生。历史快照与当前树是不同时间点；复核脚本要能表达这个区别。历史日志不属于应删垃圾。

### 4.5 本轮不做的大搬家

- 不整体移动 scene_model 或所有 tools：当前功能包内部按 model/ui/execution 分层是合理组织，不是每个都必须变成顶层目录。
- 不把 scene_camera 重并入 scene_render，以免 CPU 作者模型重新链接渲染运行时。是否减少它的安装包数量属于之后的 SDK 包装决定。
- 不把历史保存职责搬回 EditHistory；目录包含关系不是对象所有权关系。
- 不为解决“分散”新建 EditorManager、CommonContext 或 SharedEditorState。
- 不在本轮把 39 个规划 target 全部永久固定为 39 个 DLL。以后新增 target 必须说明具体依赖、独立消费者或构建验证价值；同域小 helper 默认留在本域。

后续可沿“editing / tools / persistence / desktop / extensions / application”等主题归组，但不要现在依此空建所有目录或再生成一套平行 V5 框架。Material/Flow 的模型仍归各自 tools 包，使用移动后的同一个 edit_sessions target。

## 5. 未提交 SceneObjectEdit.cpp 排版差异

用户交付记录明确存在该差异，本轮只能审阅固定提交，不能读取其本地工作树。

开始前保存实际 diff 与哈希，核对是否真只有空白变化；特别是它与 R1-A 修改同一文件，不能用 reset/checkout 覆盖。

推荐：先以独立 format-only 提交固定这份差异，再做 R1-A/R1-B；或在完整保留 diff 的基础上纳入本轮新实现提交，并明确新的全套测试以新 SHA 为准。不得把未提交内容说成已经被原 43/43 覆盖，也不得为了清洁工作区删除用户修改。

若实际差异不只是排版，应先列出语义变化并纳入本轮审阅和测试范围，而不是继续沿用“格式改动”的标签。

## 6. 实施顺序与提交要求

### 第一步：固定输入与负证据

核对远端/本地 HEAD、P02 验收的祖先关系、工作区差异，读取本包、原 P02 和 P01-R1 记录。保护用户修改。

在未修正语义的实现上加入 R02-01 和 R02-05 的真实负例。不要为了让测试失败而修改产品代码制造另一种错误。记录断言、退出码、完整输出与源码 SHA。若实际结果不符合本文推导，保留反证并解释差异，不能强行制造失败。

### 第二步：功能补正提交

只修 R1-A、R1-B。沿原所有权链和唯一 gate 实现，删除被替代的错误路径，补回归。P02 当前未通过的三个旧缺陷保持原责任。不要同时开始 Material 模型。

### 第三步：S01 目录收拢提交

只移动 history/sessions 源码归属与更新路径消费者，不改纯历史算法、target 依赖、ABI、保存职责。将真实边界关系与搬迁前比较。路径变更不能被检查器当作豁免。

### 第四步：最终验收与独立证据提交

使用显式 P02 门禁；完成新增行为、P01/R1 回归、全部受影响旧产品测试、全量构建/CTest、SDK 重装和六组消费者检查；第二轮构建检查无新增工作。命令与结果必须针对最终实现 SHA。

保留原 P02 证据，新增 `dev_log/P02-R1/`。当前可变账本仍唯一，不创建第二份。状态只能是 PASS / PARTIAL / BLOCKED；本包没有授权自动进入 P03。

## 7. 验收门槛

| ID | 必须证明 |
|---|---|
| R02-01～04 | 同组件混合批次不丢替换载荷；Undo/Redo；非法组合全批失败；纯字段不解码无关插件 |
| R02-05～08 | 读取全程禁止公开业务重入修改；戳与内容一致；失败/异常释放读取准入；拥有型组件读取同样保护 |
| S01-01 | 原两个目录不存在，新目录有唯一实现；旧到期精确路径仍不存在 |
| S01-02 | target 允许边、禁止边与搬迁前一致；基础/模型真实负例仍按指定规则失败 |
| S01-03 | 安装 include/库名/package 不变，消费者只从安装 SDK 获取产品头/库 |
| S01-04 | 原 scope 编译负例、P01/R1 证据和 P02 证据仍可按各自 SHA 验证 |
| S01-05 | 新路径记录唯一；原收据没有被重写；未来阶段不会重建旧目录 |
| G01 | 未提交改动已被保全并纳入可追踪交接；最终成绩指向新实现 SHA |

不要求测试恰好变成某个计数。不得删断言、把真实缺陷写成 WILL_FAIL、改默认行为静默跳过、或让 negative case 因缺包而失败。

## 8. 可直接发送的实施指令

> 执行本包的 P02 R1-A/R1-B 和 S01，停在 P02，未授权 P03。先保护未提交排版差异并建立真实负证据；修混合批次旧 scratch 覆盖新组件，以及编码读取缺少全程稳定准入。维持唯一历史、SessionState 和 SessionStore，不引入第二套状态或框架。随后独立提交将 editor/history、editor/sessions 移到 editor/editing/history、editor/editing/sessions，保留 target、include、ABI 和包名，补齐所有路径检查、消费者和历史证据解析。不改 main，不 force-push，不删除用户内容，不顺手修 C01/C03/C04。最终以显式 P02 门禁重新构建、运行回归、重装 SDK、验证消费者并独立提交证据，正常推送后等待复审。

## 9. 固定源码依据

以下链接固定到审阅 HEAD；文件内函数名用于定位。此处列的是本轮实际读取的代码/记录，不代表全仓库穷举。

- [S1] [P02 验收说明](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/dev_log/P02/README.md)
- [S2] [SceneSession.cpp](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/tools/scene/model/src/SceneSession.cpp)
- [S3] [SceneSource.cpp](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/tools/scene/model/src/SceneSource.cpp)
- [S4] [SceneObjectEdit.cpp](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/tools/scene/model/src/edits/SceneObjectEdit.cpp)
- [S5] [SceneSessionData.hpp](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/tools/scene/model/src/SceneSessionData.hpp)
- [S6] [SceneSnapshot.hpp](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/tools/scene/model/include/lux/engine/editor/scene/SceneSnapshot.hpp)
- [S7] [43 项 CTest 日志](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/dev_log/P02/logs/ctest.log)
- [S8] [SceneEdit.hpp](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/tools/scene/model/include/lux/engine/editor/scene/SceneEdit.hpp)
- [S9] [真实 Scene 测试](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/tools/scene/model/test/scene_session.cpp)
- [S10] [SessionState 与 EditGate](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/sessions/include/lux/engine/editor/sessions/SessionState.hpp)
- [S11] [插件快照测试](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/tools/scene/model/test/plugin_snapshot.cpp)
- [S12] [Editor 根 CMake](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/CMakeLists.txt)
- [S13] [当前 editor_editing CMake](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/editing/CMakeLists.txt)
- [S15] [纯历史 CMake](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/history/CMakeLists.txt)
- [S16] [会话 CMake](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/sessions/CMakeLists.txt)
- [S17] [模型 CMake](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/tools/scene/model/CMakeLists.txt)
- [S18] [Transform3D 真实字段](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp)
- [S19] [P02 收据](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/dev_log/P02/receipt.json)
- [S20] [PreparedSceneReload](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/tools/scene/model/src/PreparedSceneReload.hpp)
- [S21] [旧 Registry 编辑适配 CMake](https://github.com/LUX-YU/lux-engine/blob/3a4d6ce367cfe3b9551ccde01a0de6d7088fa256/editor/editing/scene/CMakeLists.txt)
- [S14] [CMake target_sources 官方说明（3.20）](https://cmake.org/cmake/help/v3.20/command/target_sources.html)：target 的 source 与目录并非一一对应。这里只引用基础构建概念，不要求升级 CMake。
