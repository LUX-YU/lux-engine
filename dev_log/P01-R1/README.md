# P01 复审与定向补正 R1 验收

**状态：PASS，仅本轮 R1 与 P01 门禁；停在 P01 等待复审。**
输入为已推送的 `a653a89bc3cfae25fbf2e0777b2b757b93583ba5`，补正实现为
`c3652237642085bf309620ab2b5229a6d90a0508`。验收材料另作一次提交；不修改 main，不进入 P02。
`may_proceed=true` 仅表示当前门禁通过，`continuation_authorized=false` 保留下一阶段授权边界。

## 三项补正与修复前证据

| 问题 | 修复前实际结果 | 补正及最终结果 |
| --- | --- | --- |
| R01：close 调用完整 describe | 注入异常使进程 terminate，退出 86；长来源关闭将调用次数从 2 增至 3，退出 1 | 私有无分配 currentContent；关闭前后 describe 均为 1；异常标志在 prepareClose 前已打开，正常关闭成功 |
| R02：三基础目标依赖漏报 | 五个真实 CMake 负例均配置成功，漏报 | 五项均命中对应规则且非零退出；逐一修复同一夹具后配置成功 |
| R03：证据依赖生产机器路径 | 迁移根目录并禁止读取原 E 盘目录时验证失败 | 原 inventory 按原 SHA 归档；同一验证器按 archive_path/archive_log 验证，缺失和篡改均失败 |

修复前日志和输入见 `logs/development/before-*`、`logs/before-portability/`。
这些结果不是“预期失败即原功能通过”。R01 的退出 86 是测试专用 terminate handler 记录真实
noexcept 越界，修正后退出 0。原生产关闭代码在这些失败运行时尚未修改。

R02 除必测五项，还覆盖合法基础依赖、合法精确外部依赖、imported 接口传递 UI、配置条件、
contracts/history 到 UI、原 Context 禁边和未解析链接目标。保留原六类架构回归。
负例图、实际配置输出、规则和修复结果一起保存在 `evidence/boundaries.json`；不把缺包或 JSON
错误算成正确拦截。完整引擎图见 `evidence/targets.json`。

R03 使用 Windows 上的隔离 Git/证据目录与路径访问拒绝模拟，不声称做过 Linux/Android 引擎构建。
测试还改变副本中的 Windows provenance 文件名，使其与归档日志名称无关，证明不再依赖
宿主平台的 `Path(log).name`。原 P01 收据、proof、日志结果没有重写。

## 行为验收

| 门禁 | 证据与观察点 |
| --- | --- |
| X01-R1-01/02 | `close-throw.log`、`close-contract.log`：长来源 32768 字节，关闭无完整描述调用 |
| X01-R1-03 | 原 owner、代次、移动、重复消费和槽位复用断言保留；新增真实历史变化后内容戳不匹配，拒绝关闭且保留对象/准入；放弃许可恢复准入 |
| X01-R1-04 | `sessions-detail.log`：独立 describe 抛出后，CallbackScope 恢复，可操作其它槽位并正常关闭 |
| X01-R1-05 | 已发布关闭与未发布候选放弃均检查历史操作 → 源 → 代码的析构顺序 |
| X01-R2-01～05 | 真实 CMake 配置、图导出、检查器、修复后重配，非字符串探针替代 |
| X01-R3-01～03 | 迁移根目录、Windows 路径模拟、缺失原始归档、篡改字节、缺失日志与恢复后验证 |
| X01-01～06、Q06～Q10 | 原会话/历史、保存基线、作用域、旧 Material/Flow/Scene 保存失败与重试全部重跑，原断言不减弱；详见 receipt 的语义映射 |
| Q45 / P01_GATE | 实际 `--stage P01` 架构图检查、负例、原 V4 静态审计、到期账本、原 AST 证据验证 |

全量 `all -j 4 -- -k 0` 通过，第二轮无新增工作。完整 CTest **37/37**，保留原 35 项，新增两项
关闭回归。`EditScope` 原合法引用、复制及移动编译用例仍运行，两个负例因 C2280 deleted constructor
失败，日志独立归档。测试数只是索引，不代替上面的具体断言。

公开会话接口改动后，已重新安装 SDK，重新配置并构建五组安装消费者；第二轮均无新增工作，
随后顺序运行 **6 项消费者测试全部通过**，包括外部插件和 GPU 消费者。最小会话消费者及会话 DLL
导入检查仍不含 UI、SceneRuntime、ProjectStorage、旧 Context 或 Vulkan。未修改 modules 公共头。

开发过程中有一次架构测试失败：CMake 将正确的 imported-hop 依赖路径折行，测试未归一化空白。
保留 `logs/development/after-ctest-01.log` 和对应 JSON；修复仅归一化空白，仍要求完整路径与正确规则。
本次最终 37 项和负例均在实现提交上重新运行。

## 所有权、到期删除和范围

- 新会话唯一 owner 仍是 SessionStore；会话来源、保存基线和准入仍由 SessionState 维护。
- 纯历史算法仍只有 `editor/history` 一份。该目录、contracts、SessionState、checkpoint 和私有桥
  在本次实现提交中均无改动；未新增第二份 current/dirty 状态或管理器。
- 原 P01 到期的 beginSave/finishSave、SaveTicket、ESaveOutcome、SAVE_STARTED、initially_saved、
  saved/save_pending/clean 等 API/字段与原调用仍已删除，五个迁移文件无转发头。R1 无新增限期桥。
- LegacyPersistenceState/LegacySaveTicket 等私有桥仍限定旧 TAssetSave、Scene/Material/FlowForge
  及其回归使用，不安装，不被新模块引用；八个旧 checkpoint 字段及桥最迟 **P12 删除**。
- `.internal/editor-redesign/` 继续作为唯一可变施工记录。本目录三个 JSON 只是本次冻结快照。
  原 V4 36 个文件和 R1 包内清单已重新核对，类型补正记录在 `P01-R1-decision.md`。

实现的新增、修改文件和 Git blob/SHA-256 见 `files.json`：15 项，未删除额外生产文件。
验收提交只追加本目录记录；不借此修改产品或原失败判定。

## 仍未通过与保留限制

| 旧缺陷 | 本轮复测 | 后续责任 |
| --- | --- | --- |
| C01 / Q31 | **FAIL，退出 1**：布局被拒后 visible 仍从 0 变 1 | P09 验证 / P12 接入 |
| C03 / Q38 | **FAIL，退出 1**：QUERY 内替换命令时过早释放代码 | P11 |
| C04 / Q42 | **FAIL，退出 1**：连接失败未阻止 create | P12 |

三项复现源码及判定均未修改，失败没有包装为通过。R1 必测无缺项或未通过项。
未实施 P02 模型、后续工作流、完整人工 IME/桌面、性能专项及 clean-clone foundation/closure 资格。
这些不属于本轮门禁；不把 P01 PASS 扩大为整个编辑器完成。

复核入口：`python dev_log/P01-R1/check_receipt.py`。它核对本次归档、Git 不变量和行为日志，
并调用修正后的原 `dev_log/P01/verify.py` 验证历史证据；不复制原 AST 验证逻辑。
`receipt.json` 保存完整命令、退出码和日志哈希；producer 路径仅作来源记录。
