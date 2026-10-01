# P10Q — 代码质量收敛、基建统一与工程边界整改

**日期：2026-09-30**  
**项目：LUX-YU/lux-engine**  
**实施分支：`codex/editor-redesign-v4`**  
**插入位置：P10 与 P11 之间；不重编号 P11–P13**  
**性质：实施指令，不是已实施补丁，也不是质量验收通过证明。**

> 本阶段的目标是：让现有 Editor 成为一个接口一致、责任内聚、合理复用基础库、能跨平台构建的 C++20 工程；不是再增加一层框架。
>
> 原则：**保留已验证的语义，改变不合理的实现形状；一份事实、一份 owner、一套实际算法；在真实边界验证，在受控内部直接工作。**
>
> 执行完 P10Q 后停止复审，不自动进入 P11。不修改 main、不重置用户改动、不删除用户数据。本文允许调整之前被限定为“本轮不动”的目录、公共头和内部 target，但不允许以质量改进为名丢失原行为。

## 0. 文档入口、事实时点与优先级

### 0.1 本次实际核验到的代码时点

| 内容 | 固定参考 | 本次核验边界 |
|---|---|---|
| lux-engine 实施分支最新验收 HEAD | `b583e7ffe20e7a1ac55c7119d6a13ac337ebb323` | 已读取分支 ref 与该提交；提交说明为 P10 R1 证据交付 |
| 对应 P10 R1 实现 | `514bca1f180e91a88545d26d5d11a63fcd7e7498` | 来自提交中的固定 FILES/README；开始实施时核验祖先关系与 receipt |
| 前一轮 P10 验收 | `b9b856477755a9247a7a6810fa880ae62151f565` | 前次结构审阅与 P10 R1 指令的调查基准 |
| lux-cxx 本次读取的 main | `bc1eab34b83b5cf8821d6319e5b2a02574dc91fd` | 已读取 ref、容器与 core/memory 代表性实际头文件 |
| imgui-node-editor | bootstrap 指向 `LUX-YU/imgui-node-editor`，使用 `imgui::node_editor` | 未核验实施机器已安装二进制的提交；须在 Q0 记录实际来源 |

**重要更新：P10 R1 已经出现新交付，不能继续按旧对话把它写成“尚未实现”。** 本文不重复要求重写它；Q0/Q1 核验当前成果并继承其回归。也不能仅凭新提交消息，就将其独立复审状态自动改为 PASS。[E01–E03]

本次做的是定向源码/原文档审查及实施方案设计。没有构建完整引擎，没有运行 lux-cxx 容器资格、真实 GPU 或 Windows/Linux 引擎测试；没有取得全仓 AST、全部文件长度、DLL 数量或性能基准。文中的复杂度推断与待测项明确区分；不提供虚构性能改善百分比。

### 0.2 阅读顺序

先读本文件，再读 `01_CONTINUING_RULES.md`；具体来源和可复核文件在 `02_SOURCE_AUDIT.md`。`reference/` 中两份旧审阅逐字保留，只作历史输入；与本文件不一致处，以本文件和最新实际实现为准。

本文件已包含工作顺序、类型关系、目录目标、删除清单、性能计划、冗余校验证明要求和验收矩阵，不允许只按聊天摘要施工。

### 0.3 与后续阶段的关系

```text
P10 主体 + 已提交的 P10 R1
    → P10Q：核验既有补正并实施质量收敛
    → P11：命令与扩展（继承 P10Q 质量规则）
    → P12：完整用例、唯一产品切换、剩余旧框架清零
    → P13：最终全矩阵资格
```

历史规范中 `docs/editor-redesign/receipts/` 是旧交付位置。继续使用 `.internal/editor-redesign/` 作为唯一可变施工材料，阶段末冻结 `dev_log/P10Q/`；不得建立第二套独立变化的账本。长期规范可进入项目正式开发文档，并在仓库既有贡献说明/LLM 入口中引用，不将整个验收目录当开发 API。

## 1. 用户九项要求与本阶段交付对应

| 用户要求 | 当前具体切入点 | 本阶段必须交付 | 验收组 |
|---|---|---|---|
| 1. 之前的问题 | P10 R1 来源保持、手工函数表、无状态 Operation、adapters、短转发头、重公共 include | 前置补正核验；真实接口和代码归属收敛，不仅改名 | Q1–Q4，XQ01–XQ12 |
| 2. C++20 实践 | owner 特殊成员、借用/拥有回调、Result、异常边界、头文件与标准模式 | 有实际改动的逐项清单、负例、严格 C++20 构建 | Q2/Q5，XQ13–XQ17 |
| 3. 基建/lux-cxx/数据结构 | LuxObject、Process、node-editor、StableSlotMap/SmallVector/SharedBytes 等 | 固定依赖提交、使用决策和真实资格；不用库名替代性能证据 | Q2/Q5/Q6，XQ03–XQ08、XQ18–XQ24 |
| 4. 目录规范 | 模式目录、重复主题、私有头泄漏、跨工具 UI 重依赖 | 精确文件迁移，删除旧入口；同主题保留必要 target | Q3，XQ09–XQ12 |
| 5. Editor 不需要许多 DLL | 新 P07/P10 已是 STATIC；不能虚构大量新增 DLL | 实际 target/装载闭包审计；内部默认 STATIC，真实动态边界逐项说明 | Q4，XQ25–XQ27 |
| 6. 跨平台 | native 门下强制 `lld-link`、Win32 输入测试、路径/整数/生成依赖 | 修正测试分类与平台实现；Windows/Linux 实际新树构建与相关测试 | Q7，XQ28–XQ32 |
| 7. 简约抽象/模式 | 重复图输入交付、回调表、action/错误混合 | 使用具体组合、Observer、RAII、小型状态机；删除多余层 | Q2/Q5，XQ02–XQ08 |
| 8. 性能/复杂度 | 全图父环重复遍历、每帧全量目录复制、查找/分配/字节复制 | 针对性基准、明确复杂度改进、容量/地址稳定性验证 | Q6，XQ18–XQ24 |
| 9. 冗余判断 | 非空不变量、重复 lookup/describe、每层 owner 检查、异步复查 | 每处删除有有效期证明；边界检查不降低；更少重复解析 | Q5，XQ14–XQ17 |

### 1.1 不是以下任务

不重写 Lua、渲染后端、Process 执行器、ECS 或文件格式；不把三类作者模型合成万能 Document；不统一所有容器、不发明通用事务/任务/服务总线。不因加入设计模式而增加继承层级。

不提前实现 P11 动态扩展、P12 全产品工作流或任意插件热卸载。已经无需消费者的旧壳本轮删除；尚有实际旧产品消费者的其他 P12 桥不扩大范围、不改名逃避期限。数据只读兼容与源代码兼容分开：旧资产/布局读取、失败样本、用户备份继续保留。

## 2. 整体设计标准：从“分层数量”转为“自然协作”

### 2.1 一份事实对应一个真正提供者

目录版本、内容戳、保存基线、操作终态、UI 显示和 GPU 退休分别是不同事实。新类型必须说明自己拥有哪种事实，或借用谁的事实。若仅把 A 的值原样搬到 B 并改名字，优先删掉 B，而不是为 B 再建一个 adapter。

普通对象引用可用于稳定的进程内组合；只有真实多实现、外部扩展、不同副作用后端或需要返回受限能力时才引入接口。禁止“每个类默认有一个 I 接口”“所有业务都先擦成 void*”。

### 2.2 通信统一，但不是所有方法都信号化

| 性质 | 固定做法 | 不能做的替代 |
|---|---|---|
| 控件事件、提交后的事实变化、多订阅通知 | LuxObject 的 TSignal/Connection；使用准确发送时点和 payload | 新 EventBus、私有订阅表、手工反向断连链 |
| 同步读取、校验、准入，有返回值 | 具体类型方法；真正必要时使用窄角色；返回原准确 Result | 为了“全信号”模拟同步请求响应、用 out 参数当结果总线 |
| CPU/IO/编译有限异步 | `ExecutionRuntime::submit`、`TaskScope`、现有 scheduler/TaskReporter | `std::async`、私有线程池、轮询线程、第二完成队列 |
| UI 结构改变 | 原 Root/ViewHost 安全点和有界请求 | signal/draw 栈上删除对象 |
| 文件发布和 GPU 完成 | 原可靠结果/回执通道 | 将唯一结果只放进可能 FULL 的 UI 广播 |
| 纯值、身份、冻结快照 | 规则明确的 C++ 值 | 为通知而让所有值继承 LuxObject |

信号是事实通知，不是事实本身。FULL/CLOSED 必须被记录或以已有 revision 重同步；不得重发整个部分成功广播导致重复执行。正常同线程 DIRECT 通知可以标记 dirty，在下个允许点读取；QUEUED 的丢提示需要有界的重同步策略，不能因为省一次轮询丢更新。[E04–E06]

### 2.3 不把所有 Guard 视为同一种冗余

Session 的编辑准入、SaveService 的回调执行期、Runtime 的遍历保护、Root 的结构安全点保护的是不同事实。应删除的是同一对象同一事实的重复 owner/busy，不是所有 guard。

在保持接口结果的情况下，私人内部调用可复用入口已经建立的 owner/类型/存在性；跨扩展回调、异步排队、容器变化、文件 IO 或下一帧的条件必须重新证明。第 11 节给出删检查的明确规则。

## 3. 本阶段实际问题分级与处理边界

**D = 已读源码可确认；A = 需要全量盘点/测量；R = 已交付补正待本阶段核验。** 不把 A 当已复现缺陷，也不把 R 当待从零实现。

| ID | 级别 | 问题/现状 | 必须动作 |
|---|---|---|---|
| QF01 | R | 最新 P10 R1 已将 UI 来源戳组合进草稿和队列 | 核验六组原来源契约和 before 证据；保留，不重新造草稿系统 |
| QF02 | D | ProjectCatalogAccess/MaterialViewRequests/AssetOpenRequests 等手工 owner+函数表 | 提供者出正式契约，移除生产链重复擦除和错误翻译 |
| QF03 | D | ProjectStorage 已有 catalogChanged，但新 UI 主要轮询版本 | 移出真实目录能力、让通知和查询同属实际目录 owner |
| QF04 | D | TaskQueryPort 可无 revision，TaskView 会每帧复制 taskInfos | 接现有单 observer，合并变化，多个 UI 不抢 observer |
| QF05 | D | editor/adapters/project_io 混放保存执行和文件后端 | 按功能归回 persistence/storage，删目录和过期包入口 |
| QF06 | D | 两种 Publish*Operation 只有静态 start，发布本体近乎重复 | 改自由函数，公共票据交接算法只保留一份 |
| QF07 | D | SessionAccess 单别名头、微型 ViewFactory、PImpl 重 include | 删除同义入口、合理归组、收窄公开依赖 |
| QF08 | D | Material UI 为通用视口直接 PUBLIC 链 scene_ui | 提取既有 views 下的实际共享 viewport 职责，不引入 editor_common |
| QF09 | D | make*View 关闭回调把所有失败映成 BUSY | 保留可重试/终结/诊断区别，Host 不认识具体模型大 variant |
| QF10 | D | 两种 View 的图输入交付规则重复 | 在既有 editing 主题收一个小协议算法；队列与领域 payload 仍由具体 View 拥有 |
| QF11 | D | SceneSourceAccess::validate 逐节点调用父链遍历 | 全图校验改为一次迭代式颜色标记/等价线性算法；单节点重挂检查可保留 |
| QF12 | D | native 条件下强制找 lld-link 并登记 GPU 程序 | 按 native/GPU/toolchain/platform 分类，正确选择 host/target linker |
| QF13 | A | StableSlotMap、SmallVector、function_ref 等能否减少重复实现/分配 | 对候选逐一资格测试，不能按“LUX 库更快”批量替换 |
| QF14 | A | 新旧 Editor 实际 DLL/静态重复链接与全局身份 | CMake File API + 真链接/装载核验，默认静态且保留真正动态边界 |
| QF15 | A | 反复 describe/read/get_if/owner/null 检查 | 形成逐检查证明表，仅合并同一有效期内的重复检查 |
| QF16 | A | 文件粒度、循环复杂度、公共头闭包与缓存命中 | 工具盘点 + 人工职责分析；不以行数阈值判错 |

全图父环复杂度推断：当前对每个对象沿祖先链行走，长度为 N 的链可累计约 N(N−1)/2 次父查询。它是源码推断，不是当前机器计时结论。`makeTreeRows()` 则已有 first/next 表和显式栈，不能因为短或有循环就再重写一套。[E11–E13]

## 4. P10Q 内部实施顺序

下面 Q0–Q8 是**同一个新增阶段内的依赖顺序**，不是九个长期分支。每步提交可独立审阅，但阶段完成须为一个可构建整体。

| 顺序 | 工作 | 进入下一内部步骤前应成立 |
|---|---|---|
| Q0 | 固定代码/依赖/配置，盘点职责、target、文件、性能与检查基线 | 原 R1 身份可核对；用户修改保护；真实清单可复现 |
| Q1 | 核验并继承已交付 P10 R1，补缺而不重写 | UI 来源/回调/owner 原不变量均保留 |
| Q2 | 提供者契约、LuxObject 通信、任务通知与错误域收敛 | 生产 UI 不再依赖指定的手工函数表；没有新总线/任务 owner |
| Q3 | adapters 清除、共享 viewport、文件/公开头归组 | 新目录归属准确，旧文件/安装入口实际删除，禁边更新 |
| Q4 | STATIC/SHARED/安装闭包整改 | 模块依赖未合并成巨库，真实动态边界可说明，身份不跨 DSO 冲突 |
| Q5 | C++20、回调类型、重复协议、冗余检查整改 | 规则进入类型或入口；危险边界检查仍在，负例通过 |
| Q6 | 有界性能整改与 lux-cxx 选型 | 至少本文件指定的复杂度/查询/复制问题完成实测或明确限定保留 |
| Q7 | Windows/Linux 构建、测试分类、路径与平台调用 | 两个平台真实构建/相关运行；未测项不冒充 PASS |
| Q8 | 全回归、删除审计、安装资格、后续规则固化 | 最终实现 SHA 上完整矩阵与质量事实闭合，停在 P10Q |

### Q0 的交付表必须具体

使用 `tools/inventory.py` 生成只读候选清单，再结合 AST/实际编译图补全。脚本不证明 dead code、类型安全、性能或最终 target TYPE；退出 0 只表示盘点成功。

每项填：`问题ID → 现有文件/符号 → 现 owner → 新 owner → 实际调用方 → 修改/合并/删除 → 证明/测试 → 最终状态`。

记录实际使用的 lux-cxx、imgui、imgui-node-editor、lux-cmake-toolset、编译器、标准库、CRT、CMake、Vulkan/LLVM 与依赖前缀。branch 名或 README 版本不替代已安装头/库来源。若本地含用户改动，不自动切换其分支或 force-reset；只记录并建立隔离检出。

最新 P10 R1 还报告过一次 Workspace catalog 测试 Windows rename 的访问拒绝，后续重跑通过但占用来源未确定。Q0 收入已知不稳定测试清单；本阶段排查 fixture 隔离、句柄收尾及并发执行，不把重跑十次成功当作原因已消失。[E03]

## 5. Q1 — P10 R1 继承，不能因质量整改回退语义

最新交付称：NodePropertiesDraft 组合值与 based_on，属性直接走原带来源队列，删除松散 edits_/旧 base 字段；BEGIN/PREVIEW/COMMIT 前检查来源，临时失败保留，终结错误不永远堵塞队首。[E03]

本阶段必须读取实际两个 View CPP、DraftSources.hpp 与 receipt，检查：

1. 六组原 R10-R1 的输入范围和真实 before 证据存在，未用手工向模型传旧 expected 冒充 UI 测试。
2. 新修改继续让 Draft 与来源同次捕获；显示刷新不更新草稿来源，队列 BUSY 不偷偷重新 begin。
3. 关闭/重绑/History 换代后旧事件不能写新身份，Material 载荷清理仍在允许 gate 内。
4. Q2–Q6 改接口、队列和容器后，同一批行为重新跑；不能只引用原 R1 成绩。
5. 若实际已满足，不再重复做一份 R1 生产补丁；在 P10Q 的追踪表中标记“已核验并继承”。若发现差异，定向补缺并单独保留证据。

## 6. Q2 — 提供者契约与通信整改

### 6.1 项目目录：搬真实责任，不增加一个转发 Service

当前 `ProjectStorage` 同时拥有目录数组、by-id 索引、revision 及信号，新 UI 定义另一套 query 函数表。目标关系：

```text
ProjectStorage（项目 IO/发布，仍是实际写入提供者）
    └─ 组合 ProjectCatalogModel（唯一目录数组/索引/revision/通知）
            ↑
ProjectView / AssetPicker / Inspector 借用 const 查询 + changed Connection
```

以下为本阶段**建议目标类型**，不是声称当前已存在：

- `ProjectCatalogModel final : LuxObject`，归 `editor/project/` 原主题，不另建顶层 service/adapter 目录。
- 当前 UI 的拥有型 `ProjectCatalog` 值改为准确的 `ProjectCatalogSnapshot`（需要保留快照值时）；其底层可共享不可变数据，不能成为第二份可写目录。
- 将 `catalog_ / catalog_by_id_ / catalog_revision_` 的实际职责搬入 Model。项目实例身份只使用现有 domain 的值，不再分配第二个 project identity。
- 原 `catalogChanged` 的订阅者改连目录 owner 的 `changed`；删除旧目录信号的重复权威。`assetContentChanged` 是文件内容事实，仍归实际存储，不机械并入目录变化。
- `ProjectStorage::rebuildCatalog()` 改为准备完整候选并提交给唯一目录 owner；提交后在允许阶段通知，回调看不到半份索引。
- UI 未变更时不复制整个目录；通知后按实际版本重新读。同一 revision 的多个消费者可共用不可变快照。输入中的 AssetReference 仍需验证 project instance、catalog revision、AssetId/type。
- 删除 `ProjectCatalogAccess` 手工函数表和 `ProjectCatalogAdapter.hpp/.cpp` 的全部活动调用；新旧消费者都接正式目录对象，不新增同义“新版适配器”。

**禁止**仅让 ProjectCatalogModel 反向调用 ProjectStorage，原数组/index/revision 不动；那只是多一层包装。不可给所有 UI 暴露 ProjectStorage 的任意 IO/插件操作；直接连接的是已经准确抽出的目录能力。

构造函数保证必需目录对象存在，使用引用；合法“未绑定项目”由外层 `optional`/明确 Unbound 状态表达，在 UI 操作入口检查一次。权限/IO/BUSY 来自刷新或实际查询，不能等同空目录。

### 6.2 TaskView：使用原 Runtime observer，不维护第二任务库

将纯转发 `TaskQueryPort` 收敛为具有真实观察责任的 `TaskMonitor final : LuxObject`（建议名），归 `editor/tasks/ui/` 的既有主题；不新增一个任务执行 target。

Monitor 只拥有原 Runtime observer 的**唯一安装/撤销责任**、变更合并 revision 与通知，不拥有 Task、TaskId 生成器、TaskInfo 全量权威或任务终态。`snapshot()` 读原 Runtime，`requestCancel(TaskId)` 调原准确准入。

必须先盘点当前 `setTaskObserver` 的已有 owner。一次 Runtime 只注册一个观察桥位；不能每个 View 构造都覆盖上一位观察者。已有应用 observer 可作为唯一汇聚点直接通知 Monitor；如需改低层接口，仅在 Process 原 observer 位置完善明确的订阅凭据，不实现第二套通用信号系统。

observer 回调只记录变更/唤醒；在 owner 允许阶段发布 LuxObject 事实通知，避免在完成收取期间直接执行任意 UI 业务。多个 TaskView 通过 Connection 订阅。首次显示、revision 改变或 resync 才复制目录；稳定帧不得全量 taskInfos。

删除 TaskQueryPort 的 revision 函数表、View 的无条件轮询 fallback；保留真实任务失败、取消、任务历史裁剪的语义。最后视图关闭不取消应用拥有的任务，也不撤销其他消费者需要的 Runtime observer。

### 6.3 编译、发布、打开请求：不再把两个语义塞进 action+共同错误域

- 删除 `EMaterialViewAction` 驱动的万能 `request(owner,key,action)`；保留**编译准入**和**发布准入**两个准确入口，各自返回现有编译/持久化结果和操作身份。
- UI 事件可发 `compileRequested` / `publishRequested`，由显式接收者接到已有 owner；但 signal delivery 不等于业务准入。需要即时 Result 的正式方法直接调用提供者，不用返回 void 的广播模拟 RPC。
- 当前 Material 控制 owner 若只存在于 harness 局部 Requests，把真正的有限 operation 拥有/结果采用提到 `material/preview` 的具体提供者中，或复用已存在等价 owner；视图不收回 operation。
- Flow 继续使用已有 FlowCompilationService。不要为“对称”再套 FlowCompileController；只有真正缺少的发布请求由准确方法补齐。
- AssetOpenRequests 仅表达固定 AssetReference 的用户意图。P10Q 不提前实现 P12 OpenAssetOperation；将需要连接的事件用正式 LuxObject 信号连接到装配中的受限接收者即可，不保留裸 void* owner。
- 必须长期存放的同步回调，优先具体引用或经审阅的 `lux::cxx::Delegate<准确签名>`；Delegate 只解决类型配对，**不解决对象寿命**。不允许将函数表逐字段换成 Delegate 后声称完成责任整改。

### 6.4 关闭准备结果：保留事实，不让 Host 感知所有领域类型

指定修改 `DetachedView::PrepareClose`、三个 make*View 的关闭回调和 ViewHost 消费处。统一为准确的视图关闭准备结果：`Ready` 或具有明确重试分类的 `ViewCloseFailure`；错误包含 owning 诊断/域标记，不能是指向临时字符串的 string_view。

BUSY/可等待资源责任 → 可重试，保留整个已挂载 owner；明确失效并已清理 → Ready；真实拒绝/永久错误 → 保留失败和当前视图，不无限每帧重试、不谎称 BUSY。Host 不直接嵌入三模型巨大 variant，也不以 ready bool 丢掉原因。

结果结构可放原 `ViewInfo/IViewHost` 语义头，不为一枚 enum 创建一个库。关闭过程的节点/code/GPU 顺序、后批请求和视图不拥有 Session 原则不改变。

## 7. Q3 — 目录、文件、公共头与构建归属

### 7.1 目标不是统一文件长度，而是统一主题

保留现有顶层主题，不再新增 `adapters/ports/managers/common2` 等分类。建议结果：

```text
editor/
  contracts/                # 真正跨模块且轻量的身份/事实值
  editing/
    history/                # 唯一历史算法
    sessions/               # Store / SessionState / gate
    src/                    # 必要共享小协议；旧职责按账本退出
  persistence/              # SaveService / WriteCoordinator / SaveExecution
  storage/
    publication/            # 文件发布原语 + 正式 FileArtifactStore
  project/                  # 项目定义、唯一 CatalogModel
    ui/
  tasks/ui/                 # TaskMonitor、TaskView，不拥有任务
  views/
    api/                    # IViewHost / DetachedView / close preparation
    viewport/               # 真实跨工具共享的呈现与输入，不含作者业务
  desktop/
  widgets/                  # GraphCanvas、NodeCanvas、TreeRows；不含领域戳
  workspace/
    layout/
    storage/
  tools/
    scene/{model,persistence,execution,projection,interaction,ui}/
    material/{model,persistence,preview,interaction,ui}/
    flowforge/{model,persistence,compilation,interaction,ui}/
  transition/               # 仅尚有真实旧消费者的既有项，P12 到期
```

上面的 publication/viewport 是同一已有主题下的归组；不是要求每层都生成一个 DLL。`storage/publication` 的实际旧 CMake 所在目录先在 Q0 核对；如果原 file_publication 已有规范位置，优先在那里接入，避免无收益的再次搬家。

### 7.2 逐文件迁移与删除要求

| 原位置/符号 | 新归属/处理 | 必须删除或更新 |
|---|---|---|
| `editor/adapters/project_io/src/SaveExecution.cpp` 及公开头 | `editor/persistence/src/SaveExecution.cpp`；公开头 `.../persistence/SaveExecution.hpp` | 所有旧 `.../io/SaveExecution.hpp` include、source/安装条目；不留 forwarding header |
| `.../ProjectArtifactStore.cpp/.hpp` | `editor/storage/publication`，按职责改名 `FileArtifactStore` | 旧类名、旧 include、旧 project_io 包配置；文件算法与发布事实保持 |
| `editor/adapters/project_io/CMakeLists.txt` | 其执行部分并入 persistence 主题，文件部分并入 file-publication 主题 | 删除 `project_io` target/旧 alias/导出；迁完消费者后删除空 adapters |
| `editor/transition/ProjectCatalogAdapter.*` | 由正式 ProjectCatalogModel 的生产 API 取代 | adapter 文件、所有引用与安装/测试适配输入 |
| `project/ui/.../ProjectCatalogAccess.hpp/.cpp` | 真正目录值/查询归 `project/`；payload decode 仍为准确自由函数 | 手工 vtable；若 CPP 只剩 decode，移回正式 AssetCatalog 主题，不丢拖放校验 |
| `tasks/ui/.../TaskQueryPort.hpp` | TaskMonitor 实际观察职责及原 TaskView | 仅转发/裸 revision 回调；不留 using alias 保住旧名 |
| `tools/{material,flowforge}/.../Publish*Operation` | 同命名空间 `publishCompiledMaterial` / `publishFlowArtifact` 自由函数 | 无状态 class、`::start` 旧调用；不要删除真正异步 owner |
| 两份 Publish CPP 的相同 reserve/provide/cancel/ack 算法 | 在 WriteCoordinator 所属模块提供一个“已编码不可变产物准入”自由函数 | 两份事务回滚原体；领域函数仍各自验证编译结果有效性 |
| `SceneSessionAccess.hpp`、`MaterialSessionAccess.hpp` 等纯别名头 | 消费者直接用 `TSessionAccess<具体类型>`，必要别名放已有轻量语义头 | 无用别名链及空文件；不强迫 Session.hpp include Store |
| 各工具仅包装 create 的 `ViewFactory.cpp` | 合入同工具 View 创建实现；或保留一个确实容纳多个相关工厂的源 | 单函数无独立责任的编译单元；泛型 DetachedView 工厂能力保留 |
| `MaterialView.hpp` 等 PImpl 头中的私有依赖 | 私有成员依赖移 CPP，引用/指针前置声明 | 不必要重 include；值/基类/variant 所需定义不得删 |
| `SceneElement/CameraNavigation/ViewportPresentation` 的公共呈现职责 | 搬入 `views/viewport`；SceneElement 可准确命名 ViewportElement | 原 Scene UI 专用导出路径/同义壳；新旧真实调用一次性改新正式 API |
| `tools/scene/projection/src/HighlightRenderer.hpp` | 随共享 ViewportPresentation 归 viewport 私有实现 | 原位置副本；作者 SceneProjection 不搬进 viewport |
| `material_ui → scene_ui PUBLIC` | 改为实际共享 viewport 依赖 | 无关 Scene Inspector/创建/codegen/mesh-query 的传递负担 |
| 非代码文件及验收快照 | 保持历史来源位置 | 不改写 dev_log/Pxx 日志中的旧路径、旧类型名和失败样本 |

#### 保存与文件后端的 target 处理

`editor_persistence` 当前是 STATIC，仅直接依赖 edit_sessions。可以在同一个 CMake 文件定义 `editor_persistence`（纯协议）与一个 `editor_persistence_execution` STATIC（SaveExecution+Process），也可以在实际所有消费者都需要 Process 时合并。

**选择规则固定：保留纯协调器的独立无执行器消费者时采用两个 target；否则合并。** Q0 必须列出真实消费者，而不是凭想象决定。两种情况下都不保留 project_io 旧 alias，不把 FileArtifactStore 的文件 IO 塞进纯协调器。[E15]

文件后端实现 IArtifactStore 属于真实副作用多态，不是兼容壳。保留可控后端与真实后端的统一发布语义、Unknown、删除顺序和验证；不能为了清空 adapters 破坏这条边界。

#### 共享 viewport 是实际依赖提取，不是新增万能 UI 层

当前 ViewportPresentation 头的输入是 Runtime、实例、资源、相机和输出，没有作者 Session/History；它与 CameraNavigation、SceneElement 确实可同时服务 Scene 和 Material。将其真实代码放在 `views/viewport` 的一个 STATIC 目标，禁止反向依赖 scene_ui、作者模型、Inspector 或旧 Context。[E16]

作者 `SceneProjection/ScenePresentationHub` 继续留 `tools/scene/projection`，负责作者源到实例的派生；MaterialPreviewStore 继续管理材质预览业务。共享目标只持视口输入/资源/退休，不发新实例驱动、不拥有 Session。

命名和 include 变更必须同步生产、旧壳、例子、插件、生成器、CMake 和 SDK 消费者。它是本阶段明确允许的公共 C++ API 更新，不需要新增源代码兼容层。

### 7.3 小文件处置规则

少于 30/50 行、只含一个声明的文件仅是**盘点候选**，不是自动合并标准。保留具有独立使用、明确不变量或跨平台编译意义的头/源，如 CodeLease、NodeCanvas RAII、平台实现、构建输入。不能把它们都堆入 `Types.hpp` 或 2000 行 Common.cpp。

相反，若理解一个同步调用必须连续打开三份只改名字的转发文件，合并责任而不只是拼接文件。一个 800 行 View::Impl 可按“属性编辑、绘制、输入交付”拆成少量具体内部对象；不得生成十多个只访问同一个 Impl 的 friend helper。

本阶段必须交付真实行数/分类直方图和人工处置清单；不设必须减少多少文件的指标，不把测试/代码生成/平台实现计为业务碎片来刷数字。

## 8. Q4 — Editor 的 STATIC/SHARED 与 SDK 资格

### 8.1 先纠正基线认知

P07 的三个新主题和 P10 新增模块已明确为 STATIC；本阶段不能把“尚未存在的大量新 DLL”写成已经删除的成果。实际统计必须来自 CMake File API、链接命令与运行装载表，而不是 grep `add_component` 或看目录数量。[E17]

Editor 最终是一个可执行产品。内部模块用于依赖、编译和测试，不因此必须作为独立动态库。默认：

| 类别 | 目标形式 | 要求 |
|---|---|---|
| Editor 内部实现 | STATIC 优先 | 显式类型，不受 BUILD_SHARED_LIBS 偶然控制 |
| 纯公开契约/模板 | INTERFACE（确实无实现时） | 不用 INTERFACE 壳保住已废弃大 target |
| 仅拆分编译对象的内部组织 | OBJECT 可用但非默认 | 证明对象只进入预期最终映像，不反复 whole-archive |
| 必须动态装载的工具/插件 | MODULE 或真实 SHARED | 使用现有加载器要求，不因扩展名想当然切换 |
| 多 DSO 共享唯一全局状态的基础设施 | 保留有证据的共享边界，或由 host 明确注入单实例 | 对象调度、类型身份、allocator/CRT、插件 ABI 不得复制失真 |
| 测试/benchmark/harness | 不安装的可执行文件 | 不成为第二产品 |

### 8.2 不能全局 STATIC 化的原因与检查

1. 同一静态 archive 同时链接到 exe 和插件，会在不同映像里产生多个本地静态计数器、registry、TLS 或模板静态状态。必须核验 Session/Run/编译 ID 域，不只看符号能链接。
2. 在 Linux 下，静态库若要进入共享对象，必须有合适 PIC；Windows 的 DLL import/export 宏、CRT 和跨边界销毁也必须匹配。配置由 target/工具链决定，不硬编码 `-fPIC` 到所有平台。[W02–W04]
3. 不能依赖链接器恰好保留未引用的自注册对象。优先正式装配调用注册函数；必须 whole-archive 的有限目标要记录原因，不能对整个 Editor 使用它。
4. 外部 SDK “能编译静态库”不等于运行时与 host 共用同一对象/类型身份。P11 仍须外部插件验证；本阶段不发明永久 vtable 兼容层，也不从 exe 自动导出全部内部符号。
5. 若现有真实动态消费者使某库必须暂留 SHARED，在 `linkage.json` 列出消费者/符号/状态原因，后续 P11/P12 决定；不能没有证据地转 STATIC 造成 ABI 破坏，也不能仅写“保险起见”保留。

### 8.3 CMake 与安装整改

保留 lux-cmake-toolset 的正式 add_component/install 机制。实际检查宏是否正确处理 STATIC 的导出定义，是否让 private 静态依赖在安装链中可解析。**不能把 PRIVATE 当作静态依赖从最终链接消失的证明。**

构建 target、安装 component、CMake package、最终 DLL 是四种粒度。内部逻辑 target 可保留，零碎的“一小头一安装包”按真实 SDK 消费合并；不以减少 target 数破坏禁止依赖检查。

关闭/迁移旧包时，生产新前缀重装验证；原带旧头的前缀用于冻结 before，不拿残留文件帮助新消费通过。需同步 Debug/RelWithDebInfo 等实际前缀时记录；不伪称同步 include 就完成另一个平台构建。

## 9. Q5 — C++20 实践的具体整改清单

这是工程规范，不是新语法展示。目标是减少手工寿命协议和无意义间接层，不要求所有循环变 ranges，也不要求引入 C++ modules/coroutine 框架。

| 项目 | 实施要求 | 拒绝的“现代化” |
|---|---|---|
| 语言模式 | 本阶段第一方目标/安装公共头明确 C++20；`CXX_EXTENSIONS OFF`；MSVC 严格模式、GCC/Clang 相应标准模式 | 用编译器默认 C++23 掩盖 C++20 缺依赖 |
| Result | 继续 `lux::cxx::expected`，错误分类准确；返回准入/事实/诊断 | C++20 代码无条件改 `std::expected`；把错误都改 bool/BUSY |
| 拥有权 | 规则零优先；有定制析构则审查全部特殊成员；固定地址 owner 不可复制/按值移动 | shared_ptr 为了“安全”无差别扩散；默认 move 破坏 code/value 清理顺序 |
| 借用 | span/string_view/reference 只在有效范围使用；跨异步、跨帧保存必须拥有数据或固定 owner | 将 vector 改 span 后把临时借用扔进任务 |
| 必需依赖 | 构造/工厂建立完整不变量，以引用表达不可空依赖；合法 Unbound 用一个明确状态 | 每函数反复检查四个必需指针；也不把可关闭 Session 裸引用永久存起来 |
| 条件类型 | enum class、封闭 variant、optional 表达真实互斥状态 | bool active+ready+failed+bound 任意组合，或 map<string,any> 状态袋 |
| 模板 | 用 concepts/requires 表达真实约束；错误发生在明确模板边界 | 为两个固定类型搭百行 trait/CRTP 体系 |
| 通用算法 | 使用 std::ranges/算法/erase_if 中能明确表达意图的现有工具 | 为一行算法建 Adapter/Algorithm 类 |
| 初始化 | 直接初始化成员；避免默认无效对象再 init；`explicit` 防意外转换 | 工厂报告成功但内部连接/资源实际失败 |
| 异常 | 函数能失败则准确返回/传播；noexcept 只在契约确实成立时使用；扩展回调按已有边界容纳异常 | catch(...) 然后成功；给分配型查询直接加 noexcept |
| OOM | 延续项目既定 OOM 策略；普通容量/IO/业务错误仍结构化 | 为本阶段新增全仓 OOM 恢复工程；把可预期容量不足变 terminate |
| 整数/预算 | checked add/mul/cast；字节数、索引、ID 与 signed 差异明确 | 先溢出再检查；用 size_t/long 持久化跨平台协议 |
| 接口头 | self-contained；PImpl 私有依赖移 CPP；基类/值/variant 所需完整类型保留 | 依赖用户先 include 某个大头；泛用 Types.hpp 吸入整个引擎 |
| const/别名 | 修改权由正式 API 表达；对真 const 不能 cast 写 | 为绕过模型约束使用 const_cast 或暴露 Registry 指针 |
| 格式/API | 保留现有 fmt 等合适基础设施；命名一致且能描述事实 | 为 C++20 标签把所有格式化改成并不减复杂度的新调用 |

标准库中的 `std::scope_exit`、`std::move_only_function`、`std::expected` 不是 C++20 基线。使用 lux-cxx 已提供的相应能力，或既有 expected 入口。不可使用未验证的 C++23 `std::print`、ranges::to、explicit object parameter 等再通过高版本编译器默认模式蒙混过关。[L01、L05–L08]

### 9.1 回调类型选用规则

| 使用期限 | 首选 | 注意事项 |
|---|---|---|
| 立即同步执行，不保存 | 模板 Callable 或 `lux::cxx::function_ref<R(Args...)>` | 当前 function_ref 的对象构造接收 lvalue Callable&；给临时 lambda 起局部名字再借用；不返回/排队保存 |
| 固定对象与方法的非拥有调用 | 具体引用优先；必要时 `Delegate<R(Args...)>::bind<&T::method>(obj)` | 没有自动断连/寿命延长；owner 必须由真实装配保证 |
| 存入待办、唯一闭包 owner | `lux::cxx::move_only_function<R(Args...)>` | 当前 SBO 为 32 字节且有对齐/不抛 move 条件；不声称任意捕获零分配 |
| 真的需要复制闭包 | 保留 std::function 或明确可复制类型 | 不机械把所有 std::function 改 move-only |
| 对象事件订阅 | LuxObject Connection | 不能用 Delegate 替代 Connection 的取消/接收端责任 |

当前已读取的 lux-cxx 回调头只展示普通签名特化，不能假设 `Delegate<R(...) noexcept>`、`function_ref<R(...) noexcept>` 已支持。需要不抛契约时用实际提供的 API/编译期 callable 条件，或在原边界容纳异常；不要在生产里写不可实例化类型。[L05–L07]

`InspectorFields::withRead(const std::function<Status()>&)` 等即时访问是候选；真正存储的 `structural_` 则不能改成 function_ref。清理存储闭包前先解除当前活动责任，再在原 gate 作用域销毁，避免把函数包装替换变成新的重入漏洞。

### 9.2 设计模式只用于减少真实复杂度

| 已存在的实际问题 | 简约用法 | 不应引入 |
|---|---|---|
| 多 UI 观察目录/任务变化 | Observer = 原 LuxObject signal | 第二事件总线或手写 observer registry |
| 真实/可控文件后端 | Strategy = 原 IArtifactStore | 每个普通方法都配 I 接口 |
| 图输入的固定几个阶段 | 小 enum/variant 状态机+一个共享推进算法 | 一个状态一个虚类、状态 factory 和 Manager |
| 代码与 payload 清理 | RAII 组合、明确成员/替换次序、scope_exit | everywhere try/finally 式补偿状态 |
| 历史恢复 | 原 EditHistory/Memento | 每工具新 UndoManager |
| 多视口共享不可变资源 | 既有共享投影/资源 owner | 每视图克隆全世界以规避隔离 |
| 构造前验证、无失败提交 | 已有 prepare/commit 契约 | 适用于所有业务的事务引擎 |
| 无状态发布准入 | 自由函数 | 空 Operation 外壳 |

必须在工作表写出“模式减少了哪些重复和非法状态”。只增加模式名、文件或层数，不计改进。

## 10. lux-cxx 选型：按合同复用，而不是按 README 宣传替换

### 10.1 本次已核实存在的设施

固定参考 `bc1eab34…`。README 与实际头有部分不一致/过时说明，因此**以实际头、测试与当前安装 API 为准**；README 中性能倍数不直接作为本项目基准。

| 设施/头 | 可考虑的位置 | 不能错误推断的事情 |
|---|---|---|
| `SlotMap.hpp` | 需要代际 key、允许元素迁移的内部记录 | handle 稳定不等于 T* 稳定；key 不自动有 Store/Runtime 域 |
| `StableSlotMap.hpp` | 固定地址 owner、重入回调引用的非搬移记录 | erase 仍销毁对象；不替代 dispatch/reclaiming 保护；默认块大小 256 不等于业务容量 |
| `SparseSet.hpp` | 有明确上界/密度的整型 key 到小值映射 | sparse 数组随最大 key 增长；不能直接索引 UUID/hash/打包 64-bit 代际 |
| `SmallVector.hpp` | 小且通常有界的临时变更、选择、pin 列表 | inline 在对象内，不一定在栈上；增长/移动会影响引用，N 越大不一定越好 |
| `HeterogeneousLookup.hpp` | 字符串键查找避免临时 owning string | 当前头的具体类型名和 hash/equal 配对要读源码后使用 |
| `function_ref.hpp` | 同步不逃逸回调 | 不拥有 callable；不能存成员待下一帧执行 |
| `Delegate.hpp` | 必要的固定对象方法端点 | 不自动跟踪 receiver，不是 Connection |
| `move_only_function.hpp` | 延迟/唯一闭包 | 不是任意签名都无分配，不自动携带插件 CodeLease |
| `scope_exit.hpp` | 小型局部确定清理 | 不替代正式的资源拥有类型，不跨协程/异步传栈引用 |
| `SharedBytes.hpp` | 完全冻结 bytes 跨编码/发布共享 | const 视图不自动冻结其他可写 alias；owner 必须真正覆盖数据 |
| `PmrResources.hpp` | Counting/Budget/FailingMemoryResource 做定向测量/失败注入 | 只统计经其分配的内存；不是进程 RSS 或全部 GPU 峰值 |
| `CheckedArithmetic.hpp` | 容量、预算和尺寸运算 | 不额外发明同义 checkedAdd 模板 |

### 10.2 容器更换前必须填的决策卡

对每个候选记录以下九项，不允许只有“std::vector 慢所以换”：

1. key 的实际范围、稀疏度、最大值、是否不可信输入。
2. 元素是否可移动，是否包含 LuxObject/Pane、CodeLease 或自定义析构回调。
3. 外部保留的是 key、index、iterator、T* 还是 reference，分别保留多久。
4. 插入、删除、clear、reserve、swap、move 对句柄/地址/顺序的影响。
5. 业务容量/字节预算与底层 capacity 的区别，溢出/耗尽策略。
6. 是否会在遍历/析构/回调中重入；哪些 guard 是所属 owner 的责任。
7. 现有事务准备、代际语义和跨 Store 域如何保留。
8. 实际 bench 数据：比较次数、分配数、复制量、p50/p95/p99/max、内存范围。
9. 决定：替换并验证；或保留并给出具体理由。没有收益的更换应撤回。

### 10.3 三条特别约束

**稳定槽表：** 它可以减少手写 free list/代际代码，但当前 emplace 仍可能分配；reserve 也不等于达到业务限额后自动拒绝。稳定地址只有在对象尚未 erase 时成立，不能据此删除回调期间防删除保护。使用固定非移动 owner 的容器时，验证已构造对象位置和清理顺序；不要把 Pane 值塞入搬移式容器。[L02]

**SparseSet：** 当前 OffsetAutoSparseSet 会复用空号，不可拿它直接替代作者 NodeId/PinId 的已发号规则。UUID 或大 hash 不适合 raw sparse indexing。带代际的 Entity 需要完整 key 校验，不能只截取 slot 造成 ABA。[L03]

**SmallVector：** 不照搬 README“drop-in/总无额外开销”判断。当前头支持 allocator，但其注释不能作为与 std::vector 引用失效逐项等价的证明。必须跑实际 move-only payload、溢出到 heap、擦除、移回 inline 和不同 allocator 路径。旧 stable pointer 不能由于 SBO 移动而失效。[L04]

### 10.4 lux-cxx 本身发现缺陷时怎么处理

只针对本阶段确需使用的 API 修补，单独提交到 lux-cxx 工作分支并跑该组件/安装回归；lux-engine 记录精确新提交与 prefix。不得直接修改用户已有 lux-cxx 工作区、不把整套 ECS/并发容器重写为前提。

缺陷未修好时可保留满足原契约的标准容器，并记录不采用原因；这比为了“复用率”引入不合格基建更正确。不能将本轮质量整改变成新库数量或替换百分比竞赛。

## 11. 冗余判断：逐处证明，而不是全局删 assert/if

### 11.1 分类规则

| 条件 | 一次保证的位置 | 能否在后续省略 |
|---|---|---|
| 必需服务/dispatcher/不可变配置完整 | 工厂/构造发布前 | 受控私有调用链中可以；使用引用/完整对象表达 |
| 同一个 SlotKey 的 lookup/type/owner 检查 | 公共入口解析一次 | 只在无回调、无挂起、无删除/扩容失效的连续段内复用 |
| callback 是否非空 | 必需能力构造时验证；可选能力单个状态 | 不再每层检查 owner+callback 两枚松散值 |
| Session 内容/History/绑定版本 | 用户意图创建时记录、执行前校验 | 排队、回调、重新绑定、重新加载后不能沿用旧结论 |
| 文件 expected_version | 当前发布准备、最终发布前 | 外部写者可改变，不能因 earlier resolve 通过而删除 |
| UI/Runtime 外层派发与在途借用 | 所属 owner 的安全点 | 回调可能重入，不能用“单线程”删掉 |
| 消息、文件、SDK 输入长度/范围 | 对外入口 | 仍必须校验，不用 release assert 代替 |
| GPU/Run 资源已完成 | 实际回执 | 接受请求、停止请求和资源退休不能互相代替 |

**初始化保证存在，只证明当时成立。** 引用不为空不等于它会一直有效；immutable snapshot 的校验可复用，活 Session 和系统文件的校验不能跨时间复用。

### 11.2 改代码的具体形状

先在对外入口解析一次，内部 helper 接已经得到的引用和值：

```cpp
// 形状示意；不要求引入这些名称或公开新的 unsafe API。
Result<void> publicOperation(Key key, Input input) {
    auto access = resolveAndValidate(key);
    if (!access) return unexpected(access.error());
    // 以下连续段不得调用扩展回调、等待、派发或释放目标。
    return applyValidated(access->object, input);
}
```

不让 `applyValidated()` 再接 Key 然后重复 lookup。必要的断言用于开发诊断，不做运行时新分支；如果它可从外部独立调用，就仍是需校验的边界，不能假称为私有。

**禁止**新增全局 `Validated<T>`、长期 `UnsafeAccess`、可存成员的永久借用令牌来消检查。确有私有已准备对象时，它只在现有同步作用域内有效，且不能跨 callbacks/await/下一帧。

### 11.3 每处删除必须交付证明条目

`check_id, 文件/函数, 原条件, 首次证明点, 到使用点的路径, 会使条件失效的操作, 是否存在外部调用, 最终替代, 测试/断言`。

只有“第一次已经检查”而没有路径和失效事件说明，不能删除。原 owner 线程检查若属于真正的公开 SDK 入口应保留；同一受控内部主域逐 helper 存 `owner_`、比较线程 ID 可以收敛。不要为了追求零检查让 WRONG_THREAD 变 UB。

示例：`read()` 后内部提取两个属性，无扩展回调时无需再 describe 两次；但编码/copyNode 可能执行动态回调，返回后用于提交的 stamp 必须再核验。文件发布两次 digest 是外部可变边界，不是本阶段默认删项。

## 12. Q6 — 性能与数据结构：必须解决的路径、只测后决定的路径

### 12.1 必须整改的有限集合

| 路径 | 当前依据与风险 | 最小正确改法 | 不能破坏 |
|---|---|---|---|
| SceneSourceAccess 全图 hierarchy validate | 每个对象重走祖先链，深链可能二次复杂度 | 构建一次完整身份→局部索引/父索引，迭代式三色访问或等价 O(N+E) 校验；避免递归深栈 | 原循环/非法引用诊断、确定性顺序、未知引用策略、准备失败不改 live |
| Task/Project 稳定画面 | 缺观察修订时重复复制目录，多个 UI 重复相同工作 | Q2 统一通知与 immutable snapshot；同 revision 不重复 rebuild/copy | 必要 resync、FULL/CLOSED、项目关闭/切换、可解释失败 |
| 两份派生发布函数 | 从已有 SharedBytes 再生成同内容 vector，且事务算法重复 | 共用一个准入算法；论证 EncodedArtifact 接受拥有型不可变 SharedBytes，来源可写时仍复制一次 | 真实 owning、总 retained byte 预算、cancel/unknown/FIFO/版本冲突 |
| NodeCanvasIds 长时间 churn | 已读实现只增长映射；固定视图长期创建/删除会积累历史 key | 在确认没有在途 widget ID/手势/排队事件的安全点回收映射或重建后端 context，保留显式视图状态；制定上限/代际 | 旧 UI ID 不命中新对象、作者 ID 高水位、pan/zoom、选择、正在拖拽的输入 |

全图算法示意（只说明复杂度，不替代真实编码/错误处理）：

```text
按已验证对象顺序建立 index 和 parent_index
color = unseen（长度 N）
每个 unseen 起点迭代沿 parent_index 行走
    遇 visiting → cycle
    遇 done 或无父 → 该条路径全部置 done
每个节点/父边只进入有限次
```

单次 reparent 的 `createsParentCycle()` 仍可在局部 O(height) 路径使用；它和一次性全图验证不是完全相同的入口。不要为了“唯一算法”让所有单节点变更也全图重建，或重复保留两份全图验证。

SharedBytes 优化不能把 mutable vector 的 const 别名直接当 frozen。准备阶段转换到明确的不可变 owner 后共享；发布操作保留 bytes 的 owner 直到实际终态。若测量显示这条复制不构成有效成本且接口改动更重，可以在 Q0 冻结决策中保留复制并说明，但两个相同事务算法仍需收敛。

### 12.2 必须测量，但不强制替换的数据结构

| 候选 | 先确认的事实 | 可采用决策 |
|---|---|---|
| SessionStore / ViewHost / RunStore 槽位 | 现实现是否已经是有界 O(1) 查找；是否需稳定 T*；多阶段发布语义 | 仅在减少真实重复且保护完整时用 StableSlotMap；不默认重写已通过的 Store |
| FlowCompilationService 默认小容量记录查找 | 容量、访问频率、迭代 locality、指针保留 | 16 项连续扫描可能优于散列；无收益可保留 |
| FlowInteraction 选择/同步 | 当前选择数量 S 与节点数量 N，是否每次重新 capture 后再按 selection 全扫 | 构建一次每快照 lookup 或选择集合，目标避免 S×N 重扫；缓存键必须包含来源 |
| Outliner 内容读取 | 已有 UUID→index map、parent 查询成本、每帧是否重建 | 不重做已有线性 TreeRows；只去除重复构图/标签转换，collapsed key 不依赖可改显示标签 |
| 小变更/pin/selection 列表 | 实际长度分布及引用生命周期 | 测过后选 SmallVector 的 N；不是每个 vector 统一 N=8 |
| 未知组件/资源目录 lookup | 数据规模、负查询频率与生命周期 | 增加由唯一对象表派生的索引；不要形成第二份对象权威 |
| WriteCoordinator takeReady/record 扫描 | 有界记录数 R、lane 数 L、实际查找/排序次数 | 先计数/建小索引；不能修改同目标先后与 Unknown 语义 |
| SceneProjection 的整快照重建 | source 规模、内容更新频率、同步开销、峰值副本 | 测量并公开；无证据不在本阶段发明全新组件级增量投影 |

### 12.3 基准设计与明确出口

必须记录输入规模、构建配置、机器/线程、完整源码/依赖 SHA、预热、轮次、每次样本、p50/p95/p99/max、分配及复制量。以下规模是本阶段测试输入建议，不是现有测量结果：

| 基准 | 输入 | 主要观察 | 出口 |
|---|---|---|---|
| BQ1 层级校验 | 深链/宽树/多个根/有环，N=1k/10k/50k（容量允许） | parent lookup 次数、时间、额外内存、栈深度 | 每节点/边有限访问，无 N² 增长；实际错误结果与旧有效语义一致 |
| BQ2 稳态 UI 目录 | 1/2/8 个视图，1k/10k 资产/任务，1000 稳定 update | catalog 构建/复制次数与 allocation | 无 revision 变化时零全量重建/复制；变化后有限合并刷新 |
| BQ3 图 UI churn | 反复创建/删除/Undo/重选/重绑，含 10k 轮与大 ID | live 映射/历史映射容量、旧事件命中、操作尾延迟 | 有界回收策略成立，旧 UI 身份不复活 |
| BQ4 编码发布 | 1/16/64 MiB 冻结 payload，与实际预算匹配 | 新增字节复制、保活峰值、发布/取消 | 不减少正确性来省 copy；改 SharedBytes 后 owner 生命周期和预算真实 |
| BQ5 记录/小容器 | 容量 16/64/256，选取实际热路径 | lookup/erase/迭代、地址稳定、SBO 分配 | 无收益不替换，决策及对照完整 |

**不要用测试进程启动总耗时代表热路径，也不要把 PMR 覆盖的局部分配说成全部堆或 GPU 内存。** CountingMemoryResource 只计通过它的流量；容器若还用默认 allocator，必须用覆盖该路径的计数手段，不能报零分配。[L09]

不设“必须快 20%”这类无基线指标。必须修掉已证明的二次复杂度、稳定帧重复全量复制、无界 UI 映射等指定问题。其他更换以同机对照决定；回退超出预先记录的噪声区间应解释并修复/回滚，不能用平均值掩盖尾部卡顿。

## 13. Q7 — 跨平台性：代码、构建、测试三层同时检查

### 13.1 当前可确认的构建问题

`tools/scene/ui/CMakeLists.txt` 在 `LUX_EDITOR_BUILD_NATIVE_TESTS` 内创建 GPU/Flow 综合程序并无条件 `find_program(... lld-link REQUIRED)`。因此，开启普通 native 测试会额外要求 Windows 风格 linker，而且 CPU 与 GPU/toolchain 能力没有正确分层。[E10]

整改为真实能力组合：

- 不涉及工具链/GPU 的测试只需 native 条件即可配置、构建、运行。
- 真实 GPU 测试受 GPU/desktop 能力约束，但正式生产视图仍必须编译，不是关掉整个功能包。
- Flow 编译/链接测试在 toolchain 能力组，按实际生成目标三元组、对象格式、链接参数选 backend。
- Windows native input 保留 `_WIN32`/WIN32 定位；Linux 对应输入路径使用该平台既有设施。没有实测的系统 IME 继续明确 NOT_RUN，不在本阶段伪造。
- 不能把 `lld-link` 字符串机械改为 `ld.lld`：COFF 与 ELF 参数/输出不同，必须读取现有 Flow linker 实现并选择正确平台分支或完善它的窄后端。

### 13.2 源码检查清单

| 类别 | 要检查的代码 | 修正边界 |
|---|---|---|
| 平台调用 | Windows.h、HWND、SendInput、LoadLibrary、POSIX dlopen/unistd 等 | 放在既有平台实现或明确 test-only 源；不泄露到领域/通用公共头 |
| 路径 | `E:/D:`、反斜杠拼路径、手工拆 Windows basename、大小写不一致 | `filesystem::path` 与平台路径规则；归档使用规范相对路径；不小写整个 POSIX 路径 |
| 文件发布 | Windows 替换/rename/remove 行为、Linux fsync/rename、权限/占用错误 | 保留真实成功事实与 durability/Unknown；不得 catch 后报成功或空目录 |
| 整数/二进制 | long/size_t/pointer 持久化、reinterpret_cast 未对齐、memcpy 非平凡对象 | 明确固定宽度/字节序；复用已有 codec，不修改磁盘协议 |
| C++20 | designated initializer 顺序、MSVC 扩展、GNU-only 语法、C++23 API | 标准模式实际编译；平台扩展限局部，不能靠另一编译器默认接受 |
| 头文件 | include 大小写、漏标准头、visibility 宏、PImpl 完整类型 | standalone 公开头编译；不靠偶然 transitive include |
| 测试脚本 | PowerShell-only 命令、`.exe` 固定名、VC 错误码、shell quoting | CMake target 文件引用、Python 跨平台路径；负例按真实错误含义判定 |
| 生成器 | host 工具/target 产物区分、生成头的真正 custom-command 依赖 | 保留 P06 生成顺序修复；新空树首次构建，不靠重跑掩盖依赖缺失 |
| 标准库/ABI | libstdc++/libc++、MSVC CRT、fmt/expected 入口 | 按声明支持范围测试，不宣称跨任意标准库稳定 C++ ABI |
| 退出与取消 | 文件句柄、TaskScope、窗口、dispatcher 收尾 | Windows/Linux 都需有清理事实；不在 owner 停泵后等待任务 |

### 13.3 本阶段最低实际验证

| 平台/配置 | 必测范围 |
|---|---|
| Windows 当前支持工具链，严格 C++20 | Editor 全量 all、第二轮无工作、当前完整测试集合；安装消费者；原双 SceneView GPU/输入与两种旧显式模式 |
| Linux 新源码检出 + 新 build tree，GCC 或 Clang 一种正式工具链 | Editor 第一方生产目标、PLAYER、native/保存文件/模型/Host CPU/平台适用 toolchain 回归；不靠缺文件的 skip 得 PASS |
| 第二编译器检查 | 修改公共头/关键容器/回调/标准语法的编译与适用 sanitizer；支持环境允许时扩至完整构建 |
| 依赖/SDK | Windows/Linux 新 prefix，独立 CMake 消费者，不读源码私有头和 build DLL |

若当前 Linux 的依赖组合不足，先完成可验证工作并准确报告 PARTIAL/BLOCKED；不能声明 P10Q 全部通过。也不自动把缺 Windows 的环境当成允许跳过原 GPU/原生输入回归。

本阶段不新增 Android Editor 或 macOS 完整桌面资格；若修改了跨平台公共基础头，必须至少保持它们已有受支持构建的声明与独立编译检查，最终完整平台矩阵仍由 P13 负责。

## 14. 验收矩阵：原行为 + 本次质量行为

**以下 XQ 编号是验收主题，不是要求一个编号一个可执行文件、一个目录或一个 target。** 复用现有 Fixture 和消费者；必要新增 test-only 源可按主题少量归组。

| ID | 输入/路径 | 必须观察 |
|---|---|---|
| XQ01 | 当前 P10 R1 六组全部继承 | 正式 UI 来源保持、BUSY 不 rebase、STALE 不覆盖；before 证据原样可读 |
| XQ02 | Material/Flow 共用交付协议，多 Preview/Cancel/冲突 | 一条历史、正确阶段续行、错误分类不丢失、两个领域无隐式互依赖 |
| XQ03 | 目录提交→多个真实 UI 通知 | 一份数组/index/revision owner；未变更不 rebuild；错误保留旧目录 |
| XQ04 | 目录关闭/切换、queued/full 通知 | Connection/owner 寿命准确，旧引用不命中新 project，resync 正确 |
| XQ05 | 两个 TaskView 同一 Runtime | 单 observer 安装，不抢占，取消事实真实；稳定帧不复制任务全集 |
| XQ06 | TaskMonitor 或最后 View 关闭、任务在途 | 任务/结果继续由原 owner 结清，通知不 UAF，不引入新完成通道 |
| XQ07 | 编译与发布两个失败路径 | 原错误域与 accepted ID 明确，不再把 publish 错误伪装 compile 错误 |
| XQ08 | 关闭 prepare BUSY/STALE/真实拒绝 | Host 保留可重试项，永久失败不无限 busy-loop，不丢真实原因 |
| XQ09 | 删除 adapters 后真实保存/产物/Workspace 发布 | 原 FIFO、冲突、Unknown、取消、Save As/ExportCopy 语义全保留 |
| XQ10 | 共享 viewport 后双 SceneView+Material 预览 | 没有 material_ui→scene_ui 重依赖；相机/高亮/资源退休仍隔离 |
| XQ11 | 公共头独立编译/精确 CMake 禁边 | PImpl 私有依赖不泄露；负例命中正确规则，修复同夹具通过 |
| XQ12 | 文件/符号/target/安装清理 | 无指定旧 alias/静态 Operation/adapters；历史字符串不误判残留 |
| XQ13 | owner 特殊成员、错误 callback 保存 | 原八项 operation 编译负例保留；新增 owner 不被隐式复制/退化复制 |
| XQ14 | 同步已验证路径、统计 lookup/describe 次数 | 没有回调的连续内部段减少重复；返回结果和外部错误契约不变 |
| XQ15 | 检查后扩展回调改变/关闭目标 | 不使用过期的“已验证”引用；原保护有效，不因精简产生 UAF |
| XQ16 | 外部输入与文件发布复查 | 错误域、长度、预算、代际、真实磁盘冲突均仍拒绝 |
| XQ17 | 异步完成/取消/重绑/外层嵌套 | 正常已准入完成不因 BUSY 丢失，内层作用域不解除外层保护 |
| XQ18 | 全图层级 N 扩大、深链/有环/非法引用 | parent 查询线性尺度，错误与原语义一致，无递归溢栈 |
| XQ19 | 容器 key 复用/最大值/跨 Store | 不把 SlotKey/AutoSparseSet 当跨域或作者身份保证 |
| XQ20 | StableSlotMap 构造失败/erase/回调中查询 | 保持唯一 owner/代际；地址稳定不被当成执行期安全替代 |
| XQ21 | SmallVector 边界/allocator/move-only 载荷 | inline↔heap、erase、移动赋值和异常清理正确，实测分配范围准确 |
| XQ22 | 图 UI 长期 churn/重绑/在途事件 | 映射可有界回收，旧 Widget ID 不触及新节点，原视图状态正确 |
| XQ23 | SharedBytes 跨 encode/publish/cancel | 无借用逃逸；销毁原源后仍安全；预算/计数不双减/漏减 |
| XQ24 | BQ1–BQ5 同机基准 | 复杂度/分配/复制/延迟记录可复现，保留无收益决策与回退 |
| XQ25 | 实际 target TYPE / 链接闭包 | 内部默认 STATIC；每个 SHARED/MODULE 有真实消费者和理由 |
| XQ26 | exe + 动态消费者/已存在插件 | 共同基础身份/registry/allocator 不因静态多份而冲突；旧 ABI 不误接受 |
| XQ27 | 新 prefix 安装/导出/PIC | 无 build/source 泄漏，静态私有链接依赖完整，例子和生成器可用 |
| XQ28 | 仅 native，无 Flow linker/GPU | CPU 配置/构建/运行成功；不是把所有模型测试关掉 |
| XQ29 | Windows/Linux 实际工具链 | 正确对象格式/链接参数与文件行为，不能换名字假装可跨平台 |
| XQ30 | Unicode/空格/路径大小写/权限 | 不以读取失败当缺失，不改变资产格式，不借绝对路径取证据 |
| XQ31 | 完全新 build tree 生成顺序 | 首次失败保留，依赖真正修正；第二轮无工作不能替代首轮成功 |
| XQ32 | native/GPU/toolchain/SDK 分类 | 显式运行模式/命令/设备；IME 未实测仍 NOT_RUN |
| XQ33 | 全部既有 Q/X 与旧失败记录 | 无删行为/弱化断言；C01/C03/C04 原历史保持，实际新结果分别记录 |
| XQ34 | 质量规则门禁与证据迁移 | P10Q 不能被解析器当 P10 或未知跳过；日志缺失/篡改准确失败 |

### 14.1 测试总数规则

以最新输入 receipt/CTest 枚举为基线，不继续硬编码旧的 185 作为当前全量。新增 P10 R1 的场景必须进入原行为映射。可以将测试 API 改为新正式接口并保留等价/更强断言；不能要求“所有原测试源码字节不变”而迫使保留旧 alias。

删除旧接口相关编译失败样本在原 before 中冻结；修复后的活跃测试改为新接口正例/负例，不复制旧实现供它运行。运行统计区分 native、SDK、PLAYER、编译负例、GPU 模式，禁止重复累加同一场景冒充覆盖提升。

### 14.2 质量门禁不是关键字零匹配

- “无 adapter”只指本阶段指定的生产同义兼容路径；第三方真实后端/历史文档里出现这个词不构成失败。
- “无 std::thread”只禁 Editor 新建平行执行器；`std::thread::id`、测试线程及原 Process 实现不被机械禁用。
- “无冗余检查”需要路径证明，不按 if 数量评分。
- “更多 STATIC”要查实际装载，不能将插件功能配置 OFF 来达标。
- “现代 C++”不按 concepts/ranges/shared_ptr 出现频次计分。

## 15. 门禁与后续阶段接入

### 15.1 新阶段在构建/审计中有明确身份

将 `P10Q` 加入现有阶段顺序，位置固定在 P10 与 P11 之间。修改负责阶段识别的 CMake/脚本/规则：初始核对 `cmake/EditorArchitectureChecks.cmake`、`editor/tests/architecture/` 和 rules 的真实实现，不假设所有文件都用同一 regex。

使用明确序列表，而不是字符串排序或 `int(stage[1:])`：

```text
P00 ... P09, P10, P10Q, P11, P12, P13
```

最终配置显式 `LUX_EDITOR_MIGRATION_STAGE=P10Q`。P10Q 继承 P10 行为门槛，加本阶段质量门槛；不能识别时应失败，不允许默认回退 P10。后续 P11/P12/P13 均继承质量规则，但用自己当前的阶段身份运行。

旧 dev_log/Pxx 验证仍以各自 implementation_sha 与旧阶段配置核对。移动源文件或删除旧包后，不修改原日志或原收据让过去“看起来用了新目录”。

### 15.2 后续阶段必须携带的规范

将 `01_CONTINUING_RULES.md` 的实质内容纳入项目正式规则文档，并在实际贡献入口/LLM 实施入口引用。不要在每个阶段复制一份可独立改写的规则。

| 后续阶段 | 继承要求 |
|---|---|
| P11 | 新 command/extension 使用真实对象/窄契约，不为每类注册建 adapter；复用已验证回调类型、不可变快照和 code owner；不污染 runtime SDK；组件目录/静态边界以 P10Q 结果为准 |
| P12 | 使用已收敛服务/目录/UI；完成真实工作流和剩余桥清零，不重新创建 ProjectCatalogAdapter/TaskQueryPort/静态 Operation；唯一产品入口切换，不留永久 Old/New |
| P13 | 测最终空树构建、安装/平台/IME/性能；若仍有 P12 到期 owner/bridge，判 P12 未完成；禁止以审计文档或测试数量代替实际结果 |

P10Q 不授权自动进入 P11，也不改变 main 的合并权限。

## 16. 完成门槛、允许保留与交付

### 16.1 P10Q PASS 必须同时满足

1. 九项用户要求都有实际处置和证据，不是仅生成审计报告。
2. 本文指定的代码归属、函数表/错误语义/静态 Operation/alias/重公共依赖问题已整改，或确有源码反证表明不存在；反证要固定 SHA 和具体调用，不用主观“感觉没必要”。
3. Q1 原 R1 语义保留，模型/保存/Run/GPU/UI/身份的已验证不变量不回退。
4. 确認的全图二次验证与稳定目录全量复制得到修正；其他容器/字节复制方案有实测决策。所有新增/保留集合有明确上限或回收策略。
5. Windows 与 Linux 本阶段必测实际完成；缺环境标 PARTIAL/BLOCKED。指定实际 GPU 和原生输入回归有日志，不用旧模式冒充新 SceneView。
6. 新 prefix 安装成功，所有受影响公共头/消费者与原编译负例通过。
7. 无新增永久兼容层，无删功能凑通过，无对历史证据/用户文件的改写。
8. 后续阶段规则已进入唯一规范入口，阶段解析器和质量门禁真实生效。

### 16.2 允许保留的有理由项

可以保留：小容量有界线性扫描；具有真实 RAII/SDK 意义的短文件；未证明值得修改的整快照投影；实际插件/跨 DSO 唯一状态要求的动态库；仍有实际旧产品消费者且已登记到 P12 的其他旧接线。

这些不是豁免指定问题的通道。每项写明：保留对象、真实消费者、理由、成本/上限、何时重新评估。不能把本轮应删除的 adapters 目录或 QF02 的手工函数表全部填成“P12 再说”。

### 16.3 提交与归档

建议按 Q0–Q8 的内在依赖组织实现提交；功能修正、文件移动、容器/性能和链接变更尽可能分开，方便审查，但最终验收绑定一个完整实现 SHA。独立验收提交放在实现提交之后，避免在 receipt 中自引用它自己的 SHA。

`dev_log/P10Q/` 至少包含：

| 文件/材料 | 内容 |
|---|---|
| README / receipt | 实现 SHA、输入 SHA、lux-cxx/依赖来源、阶段状态及未测范围 |
| files / symbol-disposition | 新增、迁出、合并、删除、所有实际调用方和剩余期限 |
| communication / ownership | 信号/同步/异步选择、真实 owner、事件通知时点与断连策略 |
| check-elimination | 第 11 节逐检查有效期证明，不只列删除行数 |
| linkage / dependency | 真实 target TYPE、二进制装载、唯一身份/CRT/PIC、安装闭包 |
| container-decisions / performance | lux-cxx 选择卡、BQ 原始数据、计数覆盖范围、未采用理由 |
| test-map / commands / logs | 原新行为对应、模式/退出码/归档路径/哈希；不强求一表一份独立 JSON |
| before / development | 真失败、原始环境、首次构建/工具链/文件错误和后续修正记录 |
| known-failures | C01/C03/C04 原责任、IME 未测、catalog 测试不稳定性等不夸大结论 |

归档只依赖相对路径与固定 Git 对象。绝对开发路径可保留为命令来源，不作为唯一取证地址。缺失/篡改日志的验证器负例必须实际运行。

`ProjectBuilder.cpp` 既有差异继续保护，先记录当前实际 diff/hash，不以旧哈希强行覆盖。已知旧失败保持历史 FAIL；若同一个旧 API 由于本阶段删除不再存在，应保留原探针和新语义替代映射，不能制造空失败程序维持数量，也不能声称对应 P11/P12 责任已自动完成。

## 17. 可直接交给实施 LLM 的指令

> 本轮执行新增 **P10Q**，位于 P10 与 P11 之间，仅执行该阶段。读取本文件和后续统一规范，不按摘要猜测接口。
>
> 先核对 `b583e7ffe20e7a1ac55c7119d6a13ac337ebb323`、其 P10 R1 实现和实际工作区；P10 R1 已提交，先核验并继承，不重新写一套。记录实际 lux-cxx/依赖 SHA 与安装来源，保护 ProjectBuilder.cpp，不 reset、不修改 main。
>
> 按 Q0–Q8 完成提供者契约、LuxObject 通信、Process 复用、目录收敛、STATIC/ABI 审计、C++20、冗余检查和定向性能整改。清除指定的手工函数表、无状态 Operation、同义 alias、adapters 分类和跨工具重 UI 依赖；保留真实副作用后端、代码寿命和正确资源责任。
>
> 用一次入口验证和完整对象减少受控内部的重复判断，但不得删除跨回调/排队/文件/代际的必要复查。lux-cxx 以实际头和资格测试选用，不按名称或 README 的性能倍数批量替换；容器不能破坏稳定地址、作者身份、容量或结果寿命。
>
> 保留全部实际功能与已知失败证据。代码迁移允许改变旧测试 API，但必须有等价/更强行为映射；不靠旧 alias 保住编译，不用关闭模块/弱化测试凑数。Windows/Linux 新构建、真实 SDK、原 GPU/输入及 XQ 矩阵按最终实现执行；缺必测写 PARTIAL/BLOCKED。
>
> 显式接入 `LUX_EDITOR_MIGRATION_STAGE=P10Q`，更新后续 P11–P13 的规则引用；实现与验收分别提交并正常推送，停在 P10Q 等待复审，不自动进入 P11。

## 18. 来源与文档检查边界

源码和外部指南索引在 [02_SOURCE_AUDIT.md](02_SOURCE_AUDIT.md)，长期规则在 [01_CONTINUING_RULES.md](01_CONTINUING_RULES.md)。本文的新类型/路径/方法均为实施目标；不要在阶段报告中把它们当作已经存在的代码。

本包的文档一致性、链接和 ZIP 检查，只证明文档可交付；即使附带盘点脚本自测通过，也不代表 lux-engine 或 lux-cxx 通过任何一项工程资格。
