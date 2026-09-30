# P09 施工与 ADR

前置：41167a9bdff8c192fe990d53aa8dfa132d57b081；分支 codex/editor-redesign-v4。
ProjectBuilder.cpp 既有 SHA256 ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c，禁止修改/提交。
启动包 40 项 SHA256 全部一致；reference/v4 与 spec-v4 逐文件一致。

## A：格式与边界

- 新格式 TOML schema 1，仅写 `.lux/workspace/layouts/<32 lowercase hex LayoutId>.layout`、preferences、recovery、migration marker。label 不参与路径。原 `.lux/editor` 只读。
- DockTree 是有界扁平节点数组、显式根、二叉 split、叶槽列表，包含浮动根几何。解析后一次完整验证；LayoutPlan 自有副本，仅精确匹配 ViewRestoreKey+ViewType，保留额外 ViewInfo，不调用 provider。
- 合法未知字段通过有版本 opaque envelope 保存，未知完整文件版本拒绝改写。默认限制：16 MiB 文件、4096 节点/槽、64 深度、8 MiB opaque；目录单项损坏返回诊断和 partial，不冒充完整空目录。
- ViewInfo 从 view_api 目录迁入已有 contracts，公开 include 不变，保留 PaneTypeId 的相同身份类型；不引入新的值库或链接 UI。
- layout/recovery 共用 layout_model STATIC；workspace_store STATIC 负责文件读取、编解码接线和迁移，依赖原 editor_persistence/editor_file_publication；装配者传入原 project_io 的后端。无新 DLL、无 Root/作者模型依赖。
- Store 借用调用方的 WriteCoordinator 与 IArtifactStore；规范化验证使用原 publicationTargetKey，拒绝根外和硬链接。不同受控命名不产生不同 lane 身份。
- 协调器新增窄 REMOVE 发布动作和 provideRemoval；仍使用原 tickets/FIFO/Unknown/reconcile/容量。实际删除仅在 ProjectArtifactStore 发布阶段执行，提交前重验版本；提交回执是 missing 版本。仅在已有前序成功记录证明版本边时推进在途删除的前置版本，不无条件采用磁盘当前版本。
- Store 不私自取走协调器 ready 项、不建立第二队列；返回原 WriteTicket，应用现有 owner 调度后端。布局 outcome、preferences outcome、catalog result 各自读取。Unknown 未退休仍阻塞同 lane。
- migration 使用旧相对文件名的确定 hash ID 与原始输入摘要作为来源证明；新文件含来源，重复执行保留已迁移后的用户编辑；不一致明确 CONFLICT。所有新文件验证后才发布 marker。原始旧 TOML/INI 以 opaque 原字节保留。

## 保护与期限

旧 EditorWorkspace/Storage/WorkspaceRequest 仍仅既有 app/settings 消费者到 P12；本轮不连接产品。
C01 原完整应用 FAIL 仍归 P09/P12；C03 FAIL P11、C04 FAIL P12。P09 不修改这些复现。
P08 R1 access failure、P05 完成接收、P06 退休、P07 operation 特殊成员全部保留回归。

## 状态

A–E 实现完成；实现提交 38c88aeaf14ca27687d1baf844b4cc2d28cfcfe1。
F 独立干净检出全量构建及二次无工作、174/174 CTest、13 组 SDK 87/87、PLAYER 11/11、
原模型/保存/运行/交互/R1/R2、18 个 P09 依赖夹具及原负例、八项 operation 编译负例均通过。
两个显式 GPU 模式分别 1/1，通过实际三帧/12帧运行。旧 C01/C03/C04 仍为原 FAIL。
正在冻结 dev_log/P09 并执行只读归档门禁；最终收据状态以该门禁结果为准。
