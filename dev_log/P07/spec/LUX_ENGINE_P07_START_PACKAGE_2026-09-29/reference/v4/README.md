# LUX Engine Editor — 分阶段实施包 V4

**日期：2026-09-28**　**固定调查基准：`2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**

这套文件把 V3 的目标架构转为依赖有序的 **14 个实施阶段（P00–P13）**。不是按旧目录机械搬家：先建立内容和持久化事实，再建立运行/投影、交互/UI、工作区、扩展，最后完成产品用例与唯一入口切换。每阶段给出新增/复用类型、组合与继承、成员职责、调用方修改、旧代码删除期限和行为门槛。

## 如何依次交给 LLM

第一次提供完整包，要求先执行 P00。之后每轮只指定一个阶段，携带同一包和前阶段实际代码/收据。阶段文件包含共同硬约束；完整细粒度账本、依赖图和测试映射在 appendices/manifests，不能只给一句“按 V3 重构”。

阅读：[总执行契约](00_EXECUTION_CONTRACT.md) → 当前阶段 → [逐成员账本](appendices/B_RETIREMENT_LEDGER.md) 与 [模块决议](appendices/D_MODULES_AND_DECISIONS.md)。阶段必须实际 PASS 才进入下一阶段；PARTIAL/BLOCKED 不是继续许可。

## 总实施顺序

| 阶段文档 | 直接设计依赖 | 实施结果 |
| --- | --- | --- |
| [P00 基线盘点、反向依赖门禁与迁移账本](phases/P00_baseline_and_guards.md) | 基线 | 冻结旧功能、符号消费者与测试分组，建立迁移账本和禁边检查。 |
| [P01 历史纯化、身份、保存基线与会话唯一所有权](phases/P01_history_and_sessions.md) | P00 | 历史去保存语义；SessionStore/SessionState 成为工作副本唯一基础。 |
| [P02 Scene 作者模型、稳定对象身份与可撤销编辑](phases/P02_scene_authoring.md) | P01 | 提取无 UI 的 Scene 作者模型、稳定身份和撤销操作。 |
| [P03 Material 作者图与独立会话](phases/P03_material_authoring.md) | P01, P02 | 提取 Material 作者图与独立会话，不混入编译和预览。 |
| [P04 FlowForge 作者图与第三工具覆盖](phases/P04_flowforge_authoring.md) | P01, P03 | 迁移 FlowForge 的图、变量、函数签名和源环境。 |
| [P05 冻结保存、有序发布、Save As 与三类编码适配](phases/P05_persistence.md) | P01, P02, P03, P04 | 三类源冻结保存；统一物理写入顺序、回执、Save As。 |
| [P06 统一场景驱动、实例退休与独立 Run 会话](phases/P06_scene_runtime.md) | P01, P02 | 引擎单一场景驱动；独立 RunSession，真实 pause/step/retire。 |
| [P07 作者投影、视口资源、材质预览与 Flow 编译](phases/P07_projection_and_compilation.md) | P02, P03, P04, P05, P06 | 投影与多视口资源；材质/Flow 编译、迟到结果与背压。 |
| [P08 独立交互状态与底层 UI 离树构造/挂载协议](phases/P08_interaction_and_detached_ui.md) | P01, P02, P03, P04, P06, P07 | 交互独立；Pane/Element 离树构造、挂载与安全退休。 |
| [P09 布局、会话恢复与偏好的模型和持久化](phases/P09_layout_storage.md) | P01, P05, P08 | 布局、恢复清单、偏好和目录分开，旧格式只读迁移。 |
| [P10 独立视图、双视口与桌面宿主完整接线](phases/P10_views_and_desktop.md) | P02, P03, P04, P06, P07, P08, P09 | 接通 Scene/Material/Flow 与辅助视图、ViewHost 和真实双视口。 |
| [P11 命令事实、不可变扩展发布与工具工厂](phases/P11_commands_and_extensions.md) | P01, P05, P08, P09, P10 | 命令 query/execute 分离；不可变扩展快照和外部安装切片。 |
| [P12 产品用例闭合、唯一入口切换与旧框架删除](phases/P12_workflows_and_cutover.md) | P01, P02, P03, P04, P05, P06, P07, P08, P09, P10, P11 | 完整 Open/Reload/Close/SaveAll/Layout；唯一入口切换并清零旧框架。 |
| [P13 工程收尾、独立安装、全量资格与交付封存](phases/P13_final_qualification.md) | P12 | 跨平台、独立安装、真实 GPU/IME、容量性能与全回归封存。 |


“直接依赖”用于证明模块顺序，**实际发给 LLM 时仍按 P00→P13 串行**，不跳过表中看似无直接依赖的阶段。尤其 P05 要等三个领域，P10 要等底层离树 UI、投影和布局，P12 要等所有角色/扩展/视图可用。

## 为什么删除不是最后才做

P01 已删除历史里的保存票据/dirty 双重真相；P06 已删除旧 Runtime 动作名并迁完全部调用；P08 开始隔离旧 rooted UI 构造；各新路径从出现时就禁止反向依赖旧 Context。P12 完成最后消费者迁移后必须删除旧 EditorContext、PaneManager、SceneEditor/MaterialEditor/FlowForgeEditor、旧混合协议、old Impl/friend/TestAccess 和所有 transition。P13 若仍发现这些，说明 P12 未通过，不能延后。

## 包内材料

| 材料 | 作用 |
| --- | --- |
| [00_EXECUTION_CONTRACT.md](00_EXECUTION_CONTRACT.md) | 全阶段硬规则、交接、停点与反伪完成。 |
| [A_TYPE_INDEX.md](appendices/A_TYPE_INDEX.md) | 109 组阶段类型/角色定义；非强制每组一个类。 |
| [B_RETIREMENT_LEDGER.md](appendices/B_RETIREMENT_LEDGER.md) | 511 条限定成员/函数/协议种子与 61 个具体旧文件迁移项；P00 必须扩为实际全库盘点。 |
| [C_TEST_MATRIX.md](appendices/C_TEST_MATRIX.md) | V3 52 项 Q 场景 + 86 项本包 X 场景，标明首次完整门槛。 |
| [D_MODULES_AND_DECISIONS.md](appendices/D_MODULES_AND_DECISIONS.md) | 39 个逻辑 target 的无环依赖与对 V3 的明确执行化决议。 |
| [E_SOURCE_REFERENCES.md](appendices/E_SOURCE_REFERENCES.md) | 固定源码链接与本次读取/前序引用边界。 |
| [manifests/](manifests/) | JSON 阶段、类型、目标图、迁移种子、测试种子与收据模板。 |
| [scripts/audit_migration.py](scripts/audit_migration.py) | 只读过期文件/旧类型/过渡清单检查；不代替 AST 与运行。 |
| [V3_REFERENCE.md](references/V3_REFERENCE.md) | 保留完整目标架构原件供追踪，不再用其粗阶段顺序代替本包。 |


## 交付边界

本包仅为实施指导。没有修改 GitHub 仓库，没有运行引擎编译/CTest/GPU，没有将任何阶段标记为完成。已核对关键固定版本源码，但没有完整本地源树，因此符号账本明确标成种子；完整 inventory 是 P00 的强制门槛，不能假称此包已穷尽全部文件。所有测试 seed 都为 NOT_RUN。

本包新增 FlowForge 迁移不是扩大成新功能，而是补齐旧框架已有消费者，避免 Scene/Material 完成后无法删除 Context。其余未由现有能力支持的新功能不默认加入重构范围。
