# 五层设计的迁移与验收约束

本文件是 `00_LAYERED_DESIGN.md` 的实施展开。它不单独建立新架构，不改变用户已放宽的 Linux/性能验收范围，不自动放行 P11。

## 1. 迁移前固定的事实

- 参考 HEAD：`f7c27f9375cbf8dd8af37b30a6027a460de26213`。
- 参考实现：`3c20910d05d635078ef9021a3a3318086a792b8d`。
- 实际动工时核对分支、祖先关系、用户修改和已有新增提交；不 reset 到参考 SHA。
- 保护 `ProjectBuilder.cpp` 用户差异。物理移动时保留用户补丁及其来源记录；不能把用户改动混入资格提交，也不能为凑干净目录丢掉它。
- 旧 P00–P10Q 历史收据保持原字节，历史验证仍使用当时 Git 对象。
- 最新用户决定取消旧 50k 长测补满和当前 Linux 实测阻塞；原 PARTIAL 收据不改写，另加适用范围说明。

## 2. 从现有路径到五层的归属

以下是已知路径的目标责任。每行在动工时以实际文件清单分解，**不能把包含旧代码的整目录机械 git mv 后就宣布完成**。

| 当前路径/责任 | 目标 | 处理方式/特别限制 |
|---|---|---|
| `editor/contracts` 中编辑身份、代码保活等 | `editing` 相应语义头 | 拆出真正 UI-only 契约；唯一类型定义，不造转发头 |
| `editor/editing/history` | `editing` 历史实现 | 保留唯一历史算法和各 Session 独立历史实例 |
| `editor/editing/sessions` | `editing` 会话实现 | 保留代际、Store 域、owner、准入和关闭许可 |
| `editor/editing/include/src/scene` 的旧协议 | 不认证为新 editing 内核 | 随原消费者迁移删除；不能把 LegacyPersistenceState 搬成新基础 |
| `tools/scene/model` | `authoring/scene` | 迁正式作者模型、快照、领域 edit，不带 Runtime/UI |
| `tools/material/model` | `authoring/material` | 原 MaterialSource/MaterialGraph 仍唯一，不另建 GraphModel |
| `tools/flowforge/model` | `authoring/flow` | 保留数字身份高水位/元信息寿命，不改文件格式 |
| `editor/project` 的 Manifest/目录数据 | `authoring/project` | ProjectCatalogModel 留一份数组/索引/版本 |
| `editor/project` 的 ProjectBuilder | `activities/project` | 其任务仍走 Process，实际编译/构建走原工具链 |
| `workspace/layout` + recovery/preferences 等纯值 | `authoring/layout` | 纯计划不依赖 Root/Pane/provider 实现 |
| `editor/persistence` | `activities/persistence` | 纯协调、执行接线可保留不同 target；同一个 WriteCoordinator |
| `editor/storage` 的 FileArtifactStore/FilePublication | `activities/persistence` 的文件发布实现 | 不重新造通用文件系统；若底层已有同职责 API 则直接使用 |
| `editor/storage` 的 ProjectStorage/项目发布 | `activities/project` | 组合唯一 Catalog；原引擎读取/VFS/编解码不复制 |
| `tools/*/persistence` | 各对应 `activities/{scene,material,flow}` 或 persistence 的领域 CPP | 依实际闭包选一个唯一位置；通用 SaveService 不 include 具体源 |
| `tools/scene/execution` | `activities/scene` | RunStore 不变成第二 Runtime；CPU inspect API 不强连 GPU |
| `tools/scene/projection` | `activities/scene` | 共享作者投影是业务派生状态，不是工作台私有场景 |
| `tools/material/preview` | `activities/material` | 编译操作、预览采用按实际依赖分界；不为对称强合成一个类 |
| `tools/flowforge/compilation` | `activities/flow` | 固定对象重试、链接格式、可靠结果继续保留 |
| `workspace/storage` 和旧格式迁移 | `activities/workspace` | 数据格式兼容保留；不保留旧 WorkspaceRequest 业务混合体 |
| `tasks/ui/TaskMonitor` | `activities/tasks` | 从 tasks_ui target 移出；无 Pane/ImGui 的真实消费者验证 |
| `tasks/ui/TaskView` | `workbench/tasks` | 借用 Monitor，不抢 Runtime observer、不取消应用拥有的任务 |
| `views/api` | `workbench/desktop` 的窄 View 契约 | 纯共享身份定义另归 editing/布局值；不让 authoring include IViewHost |
| `views/viewport` | `workbench/viewport` | 不依赖 Scene 作者工具；遵循原渲染退休 |
| `widgets` | `workbench/widgets` | 原 ax::NodeEditor 后端保留，领域 stamp 留工具层 |
| `desktop` | `workbench/desktop` | 实际 Shell/Host，不能拥有 Session/Run/Save 状态副本 |
| `tools/{scene,material,flowforge}/interaction` | `workbench/{scene,material,flow}/interaction` 或相应语义文件组 | 保留 CPU target，不因路径移动加 GUI 依赖 |
| `tools/{scene,material,flowforge}/ui` | `workbench/{scene,material,flow}` | 保留草稿/显示/request 区别和源版本纪律 |
| `project/ui` | `workbench/project` | ProjectView/AssetPicker；目录 owner 不跟 UI 移动 |
| `tools/settings` 等界面 | `workbench/settings` | 只迁实际 UI，不携带旧 Context owner |
| `editor/assets` | 按实际功能分到 `activities/project` 和对应领域活动 | codec 继续用原格式；不得整体搬成新的 assets Manager |
| `editor/metadata` | 契约归对应 E1/E2/E3；装配归 application/extensions | 不把整套注册数据库搬到万能 extensions 层 |
| `editor/plugins` | 运行装载仍 engine/project/plugins；Editor 贡献安装归 application/extensions | 组件 UI 代码归具体工作台，编译角色归活动；不混成底层插件框架 |
| `editor/app`、`launcher` | `application/src`、`application/launch` | 入口组合；P12 同期切换和删除旧 owner |
| `editor/context`、旧 editor_ui、旧三大 Editor | 按既定产品切换删除 | 不搬到永久 legacy；保留确实仍在用的纯算法唯一实现 |
| `editor/transition` | 随消费者原子切换删除 | 不扩权、不新增长期 bridge；P12 必须清零 |
| `editor/tests` | `tests` 的原行为归组 | 测试引用与安装消费者随迁移，历史 logs 不改路径冒充重跑 |

## 3. 契约在哪里定义

| 契约 | 唯一归属 | 实现/消费者 |
|---|---|---|
| SessionId/ContentStamp/History/Session 管理 | editing | authoring 实现，activities/workbench 使用 |
| Scene/Material/Flow 源与纯编辑 | authoring 对应域 | 具体作者模型；工作台不复制 |
| 编码任务/保存角色/发布结果 | activities/persistence | 对应域保存实现、文件后端与 SaveService |
| 项目目录快照 | authoring/project | ProjectStorage 发布，多个 UI 共享 |
| 布局计划与其纯观察输入 | authoring/layout | activities 读文件；workbench 生成 inventory、执行 UI 挂载 |
| ViewFactory/DetachedView/挂载协议 | workbench/desktop | application 安装注册，具体工具实现 |
| Command/运行时活动注册 | activities/commands | application 固定内置和外部扩展版本 |
| 顶层退出、打开并显示、恢复整个工作台 | application 的小型用例 | 组合内容活动与工作台操作，不依赖双方的私有实现 |

发现某个旧接口把两个不同层的含义混在一起时，先拆事实/调用方向，不先建立 Adapter。

## 4. 具体的删除和保留规则

### 4.1 立即清理不带独立语义的对象

针对每个 Port/Provider/Operation/Controller，记录四件事：它拥有哪项事实、保证什么不变量、谁需要动态替换、有哪些实际调用者。四项都没有独立理由的，改为已有对象方法或命名空间函数，删除原类与转发路径。

若一个类只是保存 `T&` 并对其每个方法逐一转发，优先直接传 `T&`。若必须限制能力，只暴露相应 provider 的真实窄方法/借用视图，不能复制状态。

### 4.2 不对现有正确机制做“名字升级”

不把 `SaveService` 改成 `SaveActivityManager`；不把 `RunStore` 改成 `RuntimeCoordinator`；不把 `SceneSession` 模板化成万能 `Document<T>`；不把正确逻辑 include 为匹配目录多加五层命名空间。

### 4.3 只有在角色边界真实存在时保留多态

IEditSession 的异构销毁/描述/关闭、ISaveSource 的运行时角色、插件工厂属于必要多态候选。内置 codec 和通用小算法用静态约束。不能单凭 `virtual` 字样批量替换。

在真实插件装载边界生成一次类型擦除是可接受的；它必须封装所有权、调用约定、失效和代码寿命，而不是先写裸 `void*` 表，再在三个层增加补救类。

## 5. 推荐施工批次

每一批保持可构建；实现和证据正常追加，不压平或改写历史。

| 批次 | 实际工作 | 出口 |
|---|---|---|
| L0 | 读取实际文件/target/公开头、冻结类型与 owner 对照，确认新的五层规则 | 每个长期源码文件有唯一语义归属；未实施目录不建空壳 |
| L1 | editing + authoring 收拢 | 真实三模型 CPU 消费与历史/身份/读取回归通过 |
| L2 | activities 收拢，TaskMonitor 脱离 UI，保存/项目/编译/run 接线归位 | 无工作台依赖的活动消费者；所有可靠完成与晚读结果保留 |
| L3 | workbench 收拢，通用视口/widgets 与具体工具分开 | 两工具不互相链接，CPU interaction 可独立消费，真实 Host/GPU 回归 |
| L4 | application 根装配与新文档，旧链标记与删除；修正目标闭包 | 唯一新产品组合路径清楚，P12 待删项不认证为永久新层 |
| L5 | 在实际复用点加入/整理 concept、明确动态边界与实例化单元 | 正负编译测试、两种实际实现，模板未向整个服务树扩散 |
| L6 | Windows 构建/安装/功能与架构收尾 | 原行为映射保留，新的层次文档和实际依赖一致，停下复审 |

目录移动与行为修改尽可能分开提交以利复审。但不能为了“纯移动”留下破坏依赖的长期中间状态。

## 6. 构建和依赖检查

基于现有 CMake 架构检查扩展，不新增通用构建框架。

最低需要三张一致的表：源码/header 所属层，target 实际闭包，最终 executable/plugin 的符号/运行依赖。

**真实负例及修复正例：**

1. authoring/material 直接 include workbench/material 的私有头，应拒绝；移除后通过。
2. activities/persistence 的纯策略 target 间接链接 scene authoring 实现（而非所需共同值/角色契约），应按其 API 目标规则拒绝；对应具体保存角色 target 可以合法链接。
3. activities/workspace 引入 ViewHost 或 ImGui，应拒绝；纯 LayoutPlan 可合法使用。
4. workbench/widgets 引入 ContentStamp/具体 Session 以处理图业务，应拒绝；工具层加来源可通过。
5. TaskMonitor 通过 tasks_ui 间接引入 Pane/ImGui，应拒绝；活动 tasks 消费者可以独立编译链接。
6. MaterialView 为 viewport 引入整个 Scene UI，应拒绝；正常共享 viewport 可通过。
7. engine/modules 反向链接 Editor，应拒绝；PLAYER 实际编译输入不含 Editor。
8. PUBLIC/INTERFACE include 或 LINK_ONLY 的隐藏反向边，与直接依赖同样处理。
9. 模板策略头 include 具体后端，应被头归属审查发现；合法实例化单元可包含具体后端。

不是任意 cmake 非零都算成功。必须命中指定依赖规则，且同夹具移除非法边后可通过。

## 7. Concept 验收

每个新增 concept 都要有：真实通用算法、至少两个现实实例化用途或明确的开放扩展理由、语义律、正例与负例、编译成本说明。

应拒绝的负例：错误返回类型、不能满足 const 读取、错误异常保证（仅在契约真正需要 noexcept 处）、非拥有或临时借用对象被接口明确禁止时的误用。

**注意：不能声称所有非拥有语义都能被 concept 识别。** `span` 包在 struct 里仍可满足很多语法约束。编译器无法证明的深层寿命、原子性、预算和身份，需要真实行为测试。

不要把附件标准库演示当成产品 SDK 正例。正式资格必须 include 实际安装头，使用真实源/codec/领域 interaction。

## 8. 动态边界与静态链接验收

- 对运行时异构会话与保存角色，检查原 key/代际/代码 owner/回调撤销与执行保护。
- 确认模板内部没有再经过多层同义 vtable。
- 具体 built-in encoder 使用静态绑定不等于省掉 Process、任务取消或存储结果状态。
- 编译所有必需内部目标为 STATIC 的决定基于实际文件列表和 DSO owner，不能只根据目录。
- 一个插件和 exe 同时链接公共静态代码时，不能复制进程唯一身份/注册状态。保留最小共享 owner 或传入宿主状态。
- 不使用 whole-archive/全符号导出掩盖缺失依赖和静态注册的初始化问题。
- 安装前缀干净消费，旧包/转发头不能补上未声明依赖。

## 9. 需要保留的行为集合

按当前固定基线的真实测试映射，而不是在文档里硬编码必须达到某个新增测试数量。

- 三模型的原子批次、Undo/Redo、源身份、冻结读取与回调清理。
- Flow 已发号高水位、恢复和耗尽。
- 保存/Save As/Export Copy、同目标 FIFO、Unknown、迟到采用、回调撤销、递归保护和可靠完成。
- Run/实例退休与单步晚读取、确认、代码错误值寿命。
- 通用高亮的捕获失败重试、背压、视口隔离、编译乱序和固定对象重链接。
- BUSY 不清理有效 interaction、真实 STALE 才清理旧身份。
- 草稿/队列的 based_on 不因显示刷新和等待更新。
- 真实离树/挂载/通知/卸载、唯一 Pane owner 与代码释放顺序。
- 布局纯计划、稳定 LayoutId、文件错误分类、selected 旧恢复来源与中断重试。
- 新 SceneView 双视口、原生输入、受影响实际 GPU 路径；系统 IME 未测仍记 NOT_RUN。

目录收拢不重跑不相关的旧慢算法至任意固定样本数。保留轻量复杂度/容量/字节 owner 回归；修改性能代码时才重新测对应路径。

## 10. 交付与后续阶段

冻结 `dev_log/P10Q-structure/` 或当前唯一施工账本定义的一处结构补充，不另建第二套变化账本。交付应有：

- 新旧路径、定义、target、公开头和消费者对照；
- 抽象/纯转发删除及保留理由；
- 静态 concept 与动态边界清单；
- 实际依赖图与 forbidden-edge 结果；
- Windows 功能、SDK、适用真实 UI/GPU 记录；
- Linux NOT_RUN 与静态审查范围，原性能范围调整；
- 用户改动保全与历史 SHA 证明。

P11：命令和活动契约进入 activities；具体视图工厂契约进入 workbench；应用安装这些贡献，不另造顶层 extensions 业务层。

P12：完整 OpenAndShow/ExitEditor/RestoreWorkbench 组合进入 application；不需要 UI 的保存/关闭内容/恢复操作进入 activities。产品入口切换与旧 Context/Editor/transition 删除同阶段完成。

P13：只验证最终工程、实际支持范围和剩余风险，不给未删除旧框架再次延期。当前不要求 Linux 环境，并不等于将来可无证据宣布 Linux 支持。
