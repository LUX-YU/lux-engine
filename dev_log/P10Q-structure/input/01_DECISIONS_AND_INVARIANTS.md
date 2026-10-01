# 裁定、继承不变量与语义归属

本章在已认可五层设计之内细化实施；涉及对原文的纠正均显式标记，不把新推断当原资料结论。

## D01：纠正 ProjectBuilder 的旧归属推断

**源设计差异。** 原迁移表把 `ProjectBuilder` 归 activities/project，并描述其任务走 Process。此次固定提交的真实 `ProjectBuilder.hpp/.cpp` 表明：它拥有 `ProjectBuildConfig`，设置 name/plugins/initial_scene，`build() &&` 验证 Manifest、路径和 ScenePackage，然后返回配置。该代码没有调度 Process、调用工具链或写文件。[S04,S05]

**本施工裁定：** 这个现有纯配置 Builder 与 `ProjectBuildConfig` 一起放 `authoring/project`，保留准确的普通 Builder 类，不因为名字包含 Build 而移动到活动层。`ProjectCreation`、项目打开、发布、文件副作用和真正的构建执行请求才归 `activities/project`。这纠正的是原表中一个具体类型的职责判断，不改变五层设计。

**用户修改：** 该 CPP 存在已知未提交用户差异。只对固定 tracked 原体实施路径移动；工作区补丁另行保全并按新路径复现。不能凭历史哈希假定用户从未继续修改，也不能把 diff 内容混进资格提交。

## D02：ViewInfo 是纯观察值，关闭协议不是布局权威

已读 ViewInfo.hpp 同时包含纯身份／观察和 `EViewError/ViewResult/ViewCloseFailure/ViewCloseResult`。[S03]

目标分法：
- E0 的真实单一 `views/ViewInfo.hpp` 保留 `ViewId/ViewTypeId/ViewRestoreKey/ViewInfo`。它不 include Root/Pane/IViewHost；已有 `PaneTypeIdTag` 前置声明只为共享同一标签，不新造第二个 TypeId。
- 将宿主错误和关闭结果移入 E3 的一个实际 `views/ViewError.hpp`（若同义正式头已存在则合并在那里），供 IViewHost/DetachedView/具体 View 直接 include。
- authoring/layout 直接消费纯 ViewInfo 范围，不另建同字段 InventoryModel，也不持有活动目录。
- 不把 `ViewInfo` 中 visible/focused 的观察值误认为所有权；Host 仍然是唯一登记权威。

这是为避免在 E0 放具体关闭协议的施工细化。类型名称和错误枚举意义不变，消费者只改所需 include。

## D03：InteractionDelivery 不属于历史内核

当前共有 `deliverInput` 位于 `editor/editing/sinclude/.../InteractionDelivery.hpp`，实际处理 BEGIN/PREVIEW/COMMIT/CANCEL/COMPLETE 的工作台输入交付。[S07]

迁至 `editor/workbench/sinclude/lux/engine/editor/workbench/InteractionDelivery.hpp`，命名空间使用 `lux::editor::workbench::detail`；仅两个图工具 CPP/其直接测试 PRIVATE 可见。此为真实职责迁移，删除旧文件和旧 include 授权，不加转发别名，不让 widgets 认识 ContentStamp。

L5 在这一份现有算法上增加局部 concept；不新建状态机对象、不新增队列、不增加业务 target。

## D04：SourcePersistenceAccess 不按文件名一刀切

Scene 等模型中 `ScenePersistenceAccess.cpp`/私有访问可只是对 SessionState 的绑定、checkpoint、重载采用操作。若它不依赖 E2 保存类型、Process 或文件，则仍属 E1 作者模型的内部一致性机制。不要因为名称中含 Persistence 就把必须接触私有源/History 的机制硬搬 E2，再加跨层 friend/反射写口。

`SceneSaveSource/MaterialSaveSource/FlowSaveSource`、冻结编码工作与实际保存角色则归 E2。若一个源文件混用两种职责，切开“作者状态变化”与“外部保存服务接线”，不复制算法。

## D05：新层清洁与旧产品可运行同时成立

新正式 targets 必须满足 E0–E4。仍供旧产品使用的 `editor_context/editor_editing/editor_ui/旧三大Editor` 不认证成任何新内核层，而作为现有迁移账本的精确旧产品岛保留到指定阶段。

允许边：旧产品 → 新正式 API。禁止边：新正式 target → 旧产品目标或私有桥。现有正规 ProjectStorage 若仍通过 `editor_editing` 获得纯错误声明，必须提取真正需要的纯定义、移除该链接，而不是让整个 E2 白名单允许旧 editing。

新的文件不得新增旧岛消费者。已有缺陷仍保持原合同；若新链出现同类问题，另记新缺陷，不能挂 C03 延期。

## D06：CMake 单元与层是两种粒度

- History/Session 的 SHARED 身份 owner 不因合并物理路径而合成五个全局库。
- Scene/Material/Flow CPU 模型分别保留可安装 target。
- interaction 可位于 workbench，但保持 CPU target。
- TaskMonitor 与 TaskView 必須拆 target；旧包只要仍装真实 TaskView 就不算空壳。
- 带 GPU/toolchain 的集成测试移到 tests 的后配置入口，不反向增加生产库依赖。

## D07：什么必须做、什么可按证据不做

本轮必做：五层真实归属、TaskMonitor 分离、编辑/布局/UI契约拆分、旧源路径退出、输入交付 concept、目标闭包/安装/文档一致。

条件做：对冻结编码的进一步模板收敛，只有至少两个真实实现共享完整算法且不改变格式、预算、cleanup 语义时才做；否则记录保留原因。禁止为了满足概念数量配额添加未用类型。

默认不做：更换 Store 容器、修改磁盘 schema、再做组件级增量投影、全局 static 开关、Linux/Android 新矩阵、追求固定性能提升百分比。

## 继承不变量 I01–I18

| 编号 | 必须保持的事实 | 不能接受的近似替代 |
|---|---|---|
| I01 | 一个 SessionStore 独占会话；每个 Session 独立历史，共用唯一算法 | 把每个 Pane 变成内容 owner |
| I02 | current 从真实 History 读取，checkpoint/binding/admission 只有 SessionState 权威 | 新缓存 current/dirty 与原状态双向同步 |
| I03 | 一批 edits 原子提交一次；Undo/Redo 保留原身份和 payload | 分层后逐 edit 部分提交 |
| I04 | Node/Pin/变量已发号高水位与耗尽语义不回退 | 序列重建后重新从现存最大 ID 发号 |
| I05 | 冻结源深层可用；code/deleter 晚于数据析构 | 外层 shared_ptr<const T> 当全部寿命证明 |
| I06 | 回调读取及输入拒绝清理仍在正确 gate 中 | 把 cleanup 移到 scope 外 |
| I07 | BUSY/权限/IO 不等于 STALE/不存在 | 错误分支清空有效草稿或目录 |
| I08 | payload+based_on+phase 是同一被接纳输入 | 执行时重新取 current 冒充原始来源 |
| I09 | Accepted、Published/Unknown、Adopted 分开 | 一个 bool 表示保存全成功 |
| I10 | 同一个 WriteCoordinator、同目标 FIFO 与有界链证据 | 各活动私有写队列，或取消冲突检查 |
| I11 | 已准入完成可靠接收；内层完成不解除外层防重入 | BUSY 后丢唯一结果或重跑 encoder |
| I12 | Run 使用冻结输入、原 Runtime 单次驱动、停止不回写作者 | 每 View/Run 一个驱动循环 |
| I13 | 实例退休后单步结果仍可查到明确确认 | Registry 延迟不释放或直接 INVALID_ID |
| I14 | 视口本地输出/相机/高亮、共享重资产、实际 GPU 退休 | 复制整图模拟隔离 |
| I15 | 编译 key 完整、旧结果不冒充新结果、Flow 固定对象重链 | retry 时读取当前作者图 |
| I16 | ViewHost 唯一 Pane owner、Root 非拥有、离树挂载安全点 | parent/Root 和 unique_ptr 重复 delete |
| I17 | 布局纯计划不打开资产；selected 决定旧恢复来源 | 将全部 opaque locator 合并或 UI 工厂偷偷 open |
| I18 | 历史证据绑定原 SHA，当前资格绑定最终 SHA | 改旧日志路径或用早期通过覆盖最终改动 |

这些不变量来自原五层设计与 P10Q/P00–P10 回归；本包不声称已经重新执行它们。
