# 03　数据、执行者、结果与谓词语义

## 1. 不是成员函数和自由函数的审美争论

用户提出的关键问题是：谁是行为的承担者、处理什么数据、产生什么结果。`object.doSomething()` 不是唯一正确句法。

本轮采用三种并存的形态：

| 形态 | 适用对象 | 示例（目标表达） |
|---|---|---|
| 数据 + 算法／执行者 + 结果 | 输入与处理算法有独立生命周期／变化理由 | `compiler.compile(graph)` → compiled graph；`executor.undo(history)` → ApplyResult |
| 拥有资源的活动对象 | 本身负责准入、在途状态、句柄或资源 | `task_scope.submit(...)`、`view_host.adopt(...)`、`save_service.requestSave(...)` |
| 纯转换／判定函数 | 不需要独立状态和策略对象 | `planLayout(layout, observations)`、`encode(value)`、`checkCompatibility(...)` |

Graph 的 addNode 可以维护图本身结构；不必把它改成一个独立 NodeAdder。数据容器可有构造、查询、校验辅助和 RAII，不意味着它只能是 public POD。反之，命名为 Data 却跨线程执行闭包、采用 Session，就不是纯数据。

## 2. 当前 History 确切承担了什么

`EditHistory::Impl` 当前保存 entries、retired、cursor、StateId、revision、预算、owner 线程、phase 和 observer。`execute/replay` 不只是移动游标，而会：[S01–S03]

```text
校验身份／来源／预算
 → operation.prepare(context, budget)
 → plan.apply()
 → 改历史记录与 cursor/current
 → plan.publish(commit)
 → observer.changed
 → 退休项与 plan 清理
```

因此，不能假称“现有 History 已经只是数据”。用户提出的是对当前职责的重新划分。

## 3. 目标划分：EditHistory 与 EditExecutor

### 3.1 EditHistory

保留名字表示历史日志这一份数据，拥有：

- HistoryId、base/current StateId、revision、event 与已发号游标。
- 已接纳的操作／memento、标签、before/after、计量及 redo 分支。
- 当前 cursor、限额及实际分配的记录存储。
- 保证这份数据只能在原线程和合法阶段修改所需的**唯一内部控制元数据**。

公开只保留身份、只读 view／entry／snapshot，以及受控创建与析构。公开字段不得让外部直接修改 cursor 或 entries。

**纯数据职责不等于可复制、可随意移动或可持久化。** 条目可能含有绑定实际源和插件代码的 memento，仍应保持固定地址／线程与代码寿命约束。不能因为改叫日志，就序列化回调地址或拷贝一份可独立回放的历史。

### 3.2 EditExecutor

它执行原算法，不保存第二份 entries/current/revision。目标接口如下，仅为目标契约：

```cpp
class EditExecutor final
{
public:
    [[nodiscard]] EditResult<ApplyResult>
    execute(EditHistory& history, EditOperationPtr& operation) noexcept;

    [[nodiscard]] EditResult<ApplyResult> undo(EditHistory& history) noexcept;
    [[nodiscard]] EditResult<ApplyResult> redo(EditHistory& history) noexcept;
    [[nodiscard]] EditResult<void> clear(EditHistory& history) noexcept;
    [[nodiscard]] EditResult<void> close(EditHistory& history) noexcept;
};
```

它是原编辑算法的执行角色，不是 engine/process executor：不排线程、不管理 Task、不跨帧拥有一次撤销。可以无堆分配地构造；若实际实现没有状态，保留小算法对象或同命名空间函数均可，但项目内统一一种主要入口，不能两套算法。

选择成员形式不是因为“语法更先进”，而是用户要求清楚表达执行者。不得新增 IEditExecutor 或容器化 ExecutorRegistry，除非出现本轮没有提出的真实替代需求。

### 3.3 准入仍按 History 身份共享

两个 EditExecutor 实例访问同一 History，必须遇到同一保护。`phase` 不能只放在每个 executor 中，否则回调换一个 executor 就能绕过。

推荐保持 History 内一份不公开的 owner/phase/control 数据，只有 EditExecutor 修改。需要物理细分时可以在 private 中分 JournalStorage 与 ExecutionState，但不能多一个公开管理器或第二个忙标志。

这份控制元数据是数据一致性的一部分，不让 History 自行执行领域行为。观察通知由执行者在提交后发出；原 observer 的生命周期和通知失败契约保持，不能借本轮换成可能丢完成的广播。

### 3.4 原 Session 可以保留 undo()/redo()

Session 是工作副本的实际 owner，代表可执行编辑的主题。`session.undo()` 可以作为公开领域操作，内部持有／借用同一 History，并调用 `executor.undo(history)`。

这是聚合边界，不是为了迁移留下的兼容壳。底层 `history.undo()` 本身应删除；不能保留它承载算法，再加一个前置转发对象。

## 4. 精确迁移事项

| 当前项 | 处置 |
|---|---|
| EditHistory::execute/undo/redo/replay/clear/close | 算法迁到唯一 EditExecutor 实现，删除原变异声明和定义。 |
| EditHistory 的记录、游标、限额与查询 | 留在日志；不复制成 Executor 的“缓存”。 |
| PreparedEdit::friend class EditHistory | 调整为实际拥有 apply/publish 权限的 EditExecutor；不要公开 apply 给任意调用者。 |
| Entry / retired / PhaseGuard | 按唯一日志存储与受保护执行分配；尤其 plan、输入和 retired 清理要先于 guard 退出。 |
| SceneSession / MaterialSession / FlowSession 的调用 | 改用唯一执行者；读取、来源戳和会话准入顺序不变。 |
| Run 暂停编辑和其他 History 消费者 | S0 从真实引用展开后同批迁移；不能只改三模型而留下第二历史实现。 |
| History DLL 的身份计数 | 保留原跨 DSO 唯一域；不能挪到 header-local static 或每插件静态库。 |
| 安装头／包 | 物理路径和逻辑包不为这次语义变化全面改名；新增的必要头精确归原 provider，旧变异 API 不留 alias。 |
| 历史快照／旧验收 | 原字节不改；新活动测试用新 API 保留相同或更强行为。 |

原 `EditHistory::create()` 只构造有效日志，可以保留静态工厂；不为对称新增 HistoryBuilder、HistoryCompiler、HistoryManager。

## 5. 撤销失败与生命周期必须继续成立

撤销不是 `--cursor`。实施必须保留：

1. prepare 失败时作者源、游标、redo 和 memento 全部不变。
2. NO_CHANGE 不生成历史、不清 redo，但输入／plan 仍在正确 gate 内清理。
3. apply 的已准备提交与日志更新顺序不出现可观察半状态。
4. publish 发生于源与日志一致之后；回调重入、另一 executor 重入均被同一 owner 拒绝。
5. 截断、预算裁剪、clear、close 的条目析构在代码 pin 和保护范围内完成。
6. undo/redo、删除重建、分支不重用已公开 NodeId／PinId／对象身份。
7. 保存 checkpoint 不迁入 History；清历史与内容 dirty 的原语义不能被重新猜测。
8. close 通知与普通析构不混淆；不要求析构时重新进入用户回调。

不为了“History 数据不可变”而每次撤销复制整份日志。这里的不可随意修改是封装与准入，不是强制持久数据结构。

## 6. PreparedSessionData 的改名必须反映真实责任

当前它持有 Prepare/Reload 闭包，消费后在 owner 中创建或替换 Session。[S12] 本轮目标将其归为 `SessionPreparation`，保留已有 move-only、code owner、准入失败不消耗和一次消费规则。

worker 的解码输出可以是具体 `DecodedSkeleton`／既有 `SkeletonAsset` 拥有值；不同领域不必都继承一个 Data 接口。拥有型准备操作将这些值与实际 owner-stage 算法连接。

若沿原设计保留 `prepared.prepare(store, saves)`，名称必须清楚表示它是一次性准备操作。也可以将采用算法放现有安装执行者中；但不能为纯命名再加一层只转发的 SessionPreparationExecutor。

## 7. 快照与描述不是公共可变字段袋

### 7.1 ProjectCatalogSnapshot

目标为唯一私有不可变 Data 和只读方法：

```cpp
class ProjectCatalogSnapshot final
{
public:
    [[nodiscard]] ProjectCatalogVersion version() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::span<const AssetCatalogEntry> assets() const noexcept;
    [[nodiscard]] const AssetCatalogEntry* find(asset::AssetId id) const noexcept;
};
```

`find` 使用同一 Data 的索引，返回借用只在该快照 owner 存活时有效；需要跨任务持有则同时持快照或返回现有拥有型观察。不能调用 live Model 的 find 冒充冻结快照查询。

### 7.2 请求值与操作记录

`VResultIntent` 将动作与载荷绑定；`VWorkspaceIntent` 同理。所有小类型集中在一个相关内部语义头，不按类型数拆文件。

```cpp
struct AcknowledgeSave final { persistence::SaveId save; };
struct CancelSave final { persistence::SaveId save; };
struct ReconcilePublication final { persistence::WriteTicket ticket; };
using VResultIntent = std::variant<AcknowledgeSave, CancelSave, ReconcilePublication>;
```

上面是部分示意，不得因此删除现有其他结果动作。S0 从真实 switch 展开全部分支与载荷，再证明每个合法动作可表示、每个无意义组合不可构造。

在途操作则应保留为具有生命周期的类／明确状态组合。不能为了“数据纯化”，把 ArtifactPresentation 的资源和回调散成多个无 owner optional。

## 8. 谓词和命名清单

| 当前表达 | 问题种类 | 目标方向 |
|---|---|---|
| history.undo() | 数据名承担领域执行 | executor.undo(history)；session.undo() 可作为聚合入口。 |
| PreparedSessionData.prepare(...) | Data 名掩盖可执行／一次消费责任 | SessionPreparation；数据值与安装算法的边界明确。 |
| ResultIntent{action,target} | 动作与宾语能不匹配 | 动作专有载荷 variant。 |
| structure_request_=true/false | 布尔值隐藏添加／删除语义 | 明确 EComponentAction 或专有请求值。 |
| prepare/save/publish 都返回 bool | 可能混淆接纳与完成 | 原有结构化 Result／ticket 保留；只对实际存在的 bool 丢失事实处整改。 |
| capabilities 中的 is3D | 缺少主语和目标 | “目标对象支持 Spatial3D”“当前视口有 RayQuery3D”；不以名字推断。 |
| `snapshot.assets = ...` | 快照观察可脱离其 owner | 私有 storage + 只读访问器。 |

不得将每一个 `ensure/update/process` 按名称判错。先读实现：是查询、可能创建、推进工作还是提交事实，再按那份真实语义命名。

## 9. Concept 使用边界

Concept 约束真实算法所需表达式、返回类型和必要的 noexcept；不负责运行时目标适用性、代码寿命、线程和语义原子性。

优先保持具体数据和可静态组合算法；只在开放异构 owner／插件边界做一次擦除。不要模板化整个 Application 服务树，也不要用闭合 variant 冒充任意插件类型扩展。

可复用算法的输入从一开始就准确时，后续不需要每层重查同一事实。但跨 callback、IO、下一帧、代际变化后的复核依旧必要。
