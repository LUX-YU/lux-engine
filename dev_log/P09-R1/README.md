# P09 R1：旧布局快照的恢复来源补正

状态：**PASS（P09 R1 范围）**。仅 P09 R1，不进入 P10。

前置验收：`8bfadc34d73713bf453b45d090874bbb5d8dd399`。补正实现：
`c5bf69a2bb722741391984d5b584d3f90651eaeb`，分支 `codex/editor-redesign-v4`。
本目录是独立验收快照；原 `dev_log/P09/` 及更早快照保持不变。

## 真实修复前证据

先扩展原 workspace 测试，通过原 workspace 安装消费者 CMake，只链接本机已经安装的真实
P09 SDK；此时 LegacyWorkspaceImporter 尚未修改。不是包内草图、隔离头或伪实现。

每个输入文件先单独通过实际旧格式解析和布局验证，再测试同一目录；日志记录 Alpha、Beta
及 settings 的完整 SHA256。准备阶段没有新票据或 workspace 文件，原文件逐字节保持。

| 实际场景 | 修复前观察及退出码 |
|---|---|
| 同 PaneId/type、不同资产，选 Alpha | 两个文件单独合法；合并时 CONFLICT，detail=`legacy recovery binding`，退出 1 |
| 同 PaneId/type、不同资产，选 Beta | 同样 CONFLICT，选中后项也无法绕过全局表，退出 1 |
| 不同 key，选 Alpha | prepare 成功，但 `recovery_entries=2 selected_scope=0`，输出含未选 Beta 的资产，退出 1 |
| 空 selected | 仍遇到目录级冲突，退出 1 |
| 两份布局中断恢复用例 | prepare 已被全局冲突拒绝，退出 1 |
| 两份布局同 key、同 locator | 正向对照通过，退出 0 |

`before/commands.json`、实际源码、build.ninja、输入文件及运行日志保留；`sdk-files.json`
记录当时安装库、公共头和消费者程序哈希。复审包内 30 项长度及 SHA256 核验通过，但包内
Python 夹具验证不冒充本次 C++/SDK 的运行结果。

这证明合法输入被过度拒绝或恢复候选被扩大；不声称已经发生资产误打开或磁盘数据损坏。

## 生产改动和保持项

唯一生产行为修改在 `LegacyWorkspaceImporter.cpp`：

1. 先解析旧 settings，取得明确选中的文件名，再遍历全部旧布局。
2. 删除目录级 `locators` 表和跨文件 `legacy recovery binding` 冲突分支。
3. 仅选中文件的已识别 locator 进入 RecoveryManifest；所有合法布局继续迁移。
4. 每文件内部检查 PaneId 唯一，随后沿原几何及完整布局验证；不放过真实非法旧记录。
5. 空 selected、settings 确认缺失、选中文件缺失分别诊断，恢复为空；不默认挑首项。
6. BUSY、IO、版本冲突和坏 schema 仍返回错误，不发布默认值。

非选布局的完整原 TOML（含 locator 和未知字段）仍在原 opaque envelope，已知 locator 不塞回
新布局的活动视图配置。ViewRestoreKey 没有改名，稳定 LayoutId 没有变化。

虽然读取 settings 提前，摘要仍严格为“排序后的各布局路径/摘要，最后 settings 摘要”。
测试直接重算并比较完整 source_digest；legacy_origin、marker 因此不会因本次解析顺序改变失效。
`continueMigration` 完整算法、WriteCoordinator、来源冲突及既有发布协议没有修改。

已有同源目标和用户修改继续保留。已存在的 recovery 或完成 marker **不是覆盖授权**：
R1 不静默重写此前按目录并集产生的恢复清单；需要修复此类既存用户数据时，另作显式决定。
此次没有新增磁盘版本、公共类型、成员、target、包、DLL、队列或迁移管理器。

## R09-R1-01～06

同一新增测试源分别在 native 和安装后的 SDK 执行；原单布局 migration/collision 及其他
workspace 测试体和断言完整保留。

| 编号 | 场景与实际检查 |
|---|---|
| R09-R1-01 | r1-alpha：两份布局迁移、不同稳定 ID、原恢复 key 不变、仅 A、Beta 原字节保留；prepare 零票据/写入；正式发布后重读验证 |
| R09-R1-02 | r1-beta：独立根分别按正反创建顺序写文件，都只恢复 B；文件 ID、规范摘要及落盘内容一致 |
| R09-R1-03 | r1-extras：未选不同 key 的 Beta 不进入恢复，但布局及原 locator 无损；与 planner 保留活动 extras 的规则不混淆 |
| R09-R1-04 | r1-selection：空/无 settings/缺所选文件诊断及空恢复，正式写入后回退检查；BUSY/IO/冲突传回；坏 schema、重复 PaneId、坏 dock 拒绝 |
| R09-R1-05 | r1-resume：第一布局发布后销毁实际 Store，用户改名再重建；其余四次发布完成；ID/摘要/原字节保留；marker 不覆盖用户 recovery；真实目标来源碰撞仍拒绝 |
| R09-R1-06 | r1-control：两布局同 key 同 locator；原全部单布局、字节/预算、顺序/Unknown/冲突、权限/目录及 effects 回归 |

settings BUSY、权限码和版本冲突由明确的后端注入测试分类；另用真实磁盘上的 settings 目录
作为文件读取失败验证。两类证据分开，未将模拟错误称作实机权限拒绝。
原 rename 用例的真实 Windows ACCESS_DENIED/native_code=5 同时重跑。

## 最终工程资格

| 项目 | 本次结果 |
|---|---|
| 六组 R09-R1 | native 和真实安装 SDK 全通过；修复前五项失败及一项对照保留 |
| 全量构建 | target all/-j 4/-k 0 通过，第二轮无新增工作 |
| 完整 CTest | 180/180，原 174 项保留，新增六组；原测试体与断言另作源码核验 |
| 安装 SDK | 13 组共 93/93；原 87 项及新增六组，各组重建并二次无工作 |
| PLAYER | 11/11，全量及二次无工作，实际 target/编译单元不含 Editor |
| 依赖负例 | 原全部组通过；workspace 18 个实际 CMake 夹具含合法对照，预期拒绝和修复后结果均保留 |
| 原模型/保存/Run/交互/UI | 包含历次 R1/R2、真实编译/IO/后端绑定和历史归档门禁，均通过 |
| operation | 原八项特殊成员编译负例按 C2280 拒绝，不以缺包/缺头冒充 |
| GPU_UI | 显式模式，实际 feature.exe 三帧及退休，1/1 |
| EDITOR_SCENE_PANE | 显式模式，实际 scene_panes.exe --scene-panes，12 帧、resize 和退休，1/1 |
| 阶段门禁 | 主配置、PLAYER、架构及 package audit 均显式 P09；收据核验与归档可移植性检查通过 |

独立干净源码检出绑定实现 SHA，复用原构建树；不把它写成冷构建通过。全量 target all/-j 4/-k 0，
第二轮要求无工作；显式 `LUX_EDITOR_MIGRATION_STAGE=P09`，architecture/package audit 同为 P09。
SDK 先重新安装，再构建和运行消费者。没有修改 modules 公共头，没有 Android 构建。

GPU_UI 与 EDITOR_SCENE_PANE 使用显式 CONSUMER_MODE，并记录实际可执行命令；原两组目录名
含 GPU/ScenePane 的默认 CPU_UI 消费者仍按 CPU 统计，不充当 GPU 证据。新产品完整 GPU、IME
和布局应用资格继续归 P10/P13/P12。

归档门禁检查原测试体、准确新增集合、完整输出、文件哈希和实现 Git blob。验证过程只读归档，
不依赖生产机器的绝对路径；缺失和损坏证据必须拒绝。投影测量复用原脚本，其原始 JSON 保留，
冻结时仅修正本阶段日志路径前缀，不改测量值，不宣称迁移器性能资格。
GPU 驱动脚本首次仍引用原 P09 输出目录，在测试程序启动前因找不到 commands.json 退出。
原脚本和实际失败输出保存在 development 中；修正路径后两个显式模式重新运行通过。
这不计作 GPU 测试失败，也不以脚本修复替代真实模式运行。

## 范围和保留责任

- 五个修改文件详见 `FILES.md`；没有新增或删除生产文件。旧目录级合并逻辑完整删除，没有兼容入口。
- 唯一 History、SessionStore、Runtime、执行器、UI、P05 发布链均未改动。
- 原旧 Workspace UI 业务仅既有消费者暂留至 P12，没有扩大到新模型。
- C01、C03、C04 保持原 FAIL；本轮按原探针记录。C01 完整应用仍归 P09/P12，C03 归 P11，C04 归 P12。
- 原冷构建失败和 P06 窄修复证据保持。
- ProjectBuilder.cpp 原修改 SHA256 保持 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`，未纳入提交；main 未修改。

实现与验收分开提交，正常推送后停止在 P09，等待复审。
