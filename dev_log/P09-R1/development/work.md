# P09 R1 施工记录

范围：仅修正独立旧布局快照的恢复来源；不进入 P10。
前置：8bfadc34d73713bf453b45d090874bbb5d8dd399。
实现：c5bf69a2bb722741391984d5b584d3f90651eaeb。
ProjectBuilder.cpp SHA256：ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c，未修改或提交。

## 修复前

复审 ZIP SHA256 228a4f7a1bdac3ef7c9ebf9d730648c5ca48201724daa52839fae3a92fd02b41。
包清单 30 项逐项长度、SHA256 一致。包中草图不作为运行证据。

先扩展原 workspace.cpp 与原安装消费者，生产未改时链接当前真实 P09 SDK：

- 两份记录单独通过实际 SDK 迁移解析和布局验证，输出完整输入 SHA256。
- 同 key 不同资产，选 Alpha 或 Beta：均返回 CONFLICT / legacy recovery binding，退出 1。
- 不同 key，选 Alpha：返回 2 项恢复，包括 Beta，selected_scope=0，退出 1。
- 空选择与中断用例同样真实失败；同 key 同 locator 对照通过。
- 准备阶段协调器没有新票据，磁盘无新 workspace 目录，旧原字节不变。

上述来源、构建命令、实际退出码、源码及输出保存在 P09-R1-before；SDK 静态库、公共头和实际
消费者程序的哈希另存，没有复制隔离假实现。

## 最小补正

仅 LegacyWorkspaceImporter 改生产行为。先读取 selected，仍按原排序遍历所有布局；只从
准确文件名相符的选中快照提取 recovery。目录级 locators 表及跨文件冲突/并集合并删除。
每个文件内 PaneId 唯一；全部合法布局、非选原 locator 和未知原字节保留。
空 selected、缺 settings、缺所选文件分别诊断；IO/BUSY/版本冲突不当成不存在。

settings 在输入摘要中的位置仍在已排序布局之后；legacyId、legacy_origin、几何、ViewRestoreKey
和 continueMigration 完整协议未改。已存在的同源新文件、恢复清单、用户修改及 marker 不被覆盖。
没有新增公共类型、成员、库、协调器或状态机。

## 验证进度

本地全量构建、二次无工作及 20 项 workspace 测试通过。
正式资格从实现提交的独立干净检出执行，显式 P09；复用构建树，不宣称冷构建。
全量及二次无工作、180/180 CTest、13 组 SDK 93/93、PLAYER 11/11、原依赖/八项编译负例、
原模型/保存/Run/交互/UI 及 R1/R2 均通过。显式 GPU_UI 和 EDITOR_SCENE_PANE 分别 1/1。
GPU 脚本首次目录名沿用 P09，启动前失败，原脚本/输出保留；修正 P09-R1 路径后实跑两模式。
正在冻结验收快照并运行归档门禁；旧 P09 和 C01/C03/C04 证据不改写。
