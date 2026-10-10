# F0 — FINAL 规范入库独立验收

## 结论与身份

**F0 PASS（仅文档集成资格）。** 未实现或验收新版 Graph、Runtime、Feature、产品或先进画质算法。
独立资格在 implementation 提交后的 clean clone 执行；没有在验收检出中修改源码。

| 项目 | 身份 |
| --- | --- |
| 分支 | `codex/render-v2` |
| 批准父提交 | `7de3ddaa3f7614995745b41d73dfc98302310a4c` |
| Implementation I | `1eab572630a3a44546719238cc96795cedb2d556` |
| Verification V | 承载本报告的独立 Git 提交；V 只新增本文件，确切 SHA 由 Git 记录取得 |
| Frozen V1 | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| 独立 source | `D:/LuxQualification/render-v2-f0-1eab572630a3/source` |
| 证据目录 | `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F0/1eab572630a3a44546719238cc96795cedb2d556/` |
| 证据 manifest | `EVIDENCE_MANIFEST.sha256`，54 项 |
| manifest SHA-256 | `41c0936ead97fdff769e7800786b87d8ac7883b803cac26f8acbebd9f110a527` |

开工与独立资格时，远端基线均为获批准的 R4 验收提交。最终推送采用普通 fast-forward；
V 的身份及推送后远端 HEAD 在交付回执核对，不在报告中预填自引用 SHA。

## 实际变更

原包 44 文件全部按字节导入 `final/`；43 个 manifest 条目和 manifest 自身均与 ZIP 一致。
旧 `00_README.md` 仅前置 11 行入口说明，原正文保留为完全相同的后缀。
新增 [工作单](F0_WORK_ORDER.md)、[阶段解释](F0_STAGE_INTERPRETATION.md)、
[来源与保护映射](F0_PROVENANCE.md)。implementation 共 48 文件，无删除，无生产修改。

八项阶段解释落实用户选择：原包不改，机制、集成与业务证据分开，全部要求齐备前不关闭能力行。
新生产 API、类型、算法、target、安装规则及测试实现变更均为 0；原包中导入的 checker 仅校验文档。

## 独立资格原始结果

| 检查 | 结果 |
| --- | --- |
| 独立 clone | PASS：`--no-hardlinks --no-checkout`，detached I，无 alternates，检查前后 clean |
| tracked snapshot | PASS：implementation 与 clean clone 两次，退出码均 0 |
| 原包 checker | PASS：原脚本未改，20 chapters / 86 capabilities / 43 hashed files / 12 historical docs |
| 文件身份 | PASS：全部 44 文件的检出字节及 Git blob/mode/size 与 ZIP 一致 |
| CSV 主键 | PASS：能力 86、类型 131、要求 51、Workload 25，主键唯一 |
| CSV 引用/状态 | PASS：章节、Workload 引用有效；初始状态及空实现/验收证据列保留 |
| Legacy/RD0 来源 | PASS：66 原编号及源码锚点保留；102 处 source/operation 引用匹配冻结 tree |
| Frozen Legacy | PASS：719/719 mode/blob/size；archive 共 720 项含保护 CMake |
| 生产与历史保护 | PASS：2838 个允许范围外的 tracked 条目不变，四个 Render 子树精确一致 |
| Markdown | PASS：导入及 F0 文档围栏配对；15 个本地文件链接存在，代码示例不当作链接 |
| README | PASS：只增加入口说明，旧正文未改 |
| 用户原工作区 | PASS：HEAD/branch/status/staged/unstaged/untracked/tree/config 与快照完全一致；六文件哈希一致 |
| 文档语义审阅 | PASS：已批准五份 R2-FIX 架构修订有映射；没有借 fixture 关闭后续业务能力 |

独立外部检查器共执行 1408 项文档、来源和保护断言，全部 PASS；这不是 CTest 数量。
当前 capability `QualifiedStatus` 仍为 63 `NOT_MIGRATED`、3 `HISTORICAL_BASELINE_ONLY`、
20 `DESIGN_ONLY`，没有因为 F0 入库而新增功能 PASS。

主要原始证据：

- `qualification-commands.json`：snapshot、clone、checkout、package checker、独立检查的 argv/退出码。
- `01-` 至 `07-` 的 stdout/stderr logs：原始输出。
- `qualify_f0.py`、`qualification-results.json`、`qualification-checks.json`：独立检查器与完整结果。
- `source-references.json`、`markdown-links.json`、`document-review.json`：逐项来源、链接与语义检查。
- `preflight/`：原 ZIP/RD0、两个工作区快照、六文件原字节/哈希、实施前暂存 blob 核验。

## 历史与环境限制

R0/R1/R2/R2-FIX/R3/R4 六份历史验收报告及 `V1_KNOWN_ISSUES.md` 均保持原 blob。
R2 原始 PARTIAL、后续修复结果、R3 有限 CPU Graph、R4 单 queue/native Foundation 的范围未被重写。
transfer_idle、skinning WAR、Clang/UBSan、Linux、输入/IME 等旧记录仍按原判定保留。

V2 worktree 单独设置 `core.autocrlf=false`；原工作区配置未改。
独立 clone 在 checkout 前使用同一设置，保留 Markdown LF 与 CSV CRLF 的原始混合换行。
这不是生产或构建依赖变更；未使用隐藏源码补丁或旧 SDK。

| 未执行项目 | 本轮状态及原因 |
| --- | --- |
| Bootstrap configure / 两轮 build / CTest | NOT_RUN：F0 为批准的纯文档阶段 |
| Native GPU / validation / Sanitizer / 性能 | NOT_RUN：无生产代码变更，不能借历史 59/59 或 13/13 冒充新结果 |
| 产品与 installed SDK | NOT_RUN：产品仍 EXPECTED_UNAVAILABLE，正式切换在 F11 |
| Linux / Android | NOT_RUN：本轮没有平台构建资格 |
| 三安装前缀同步 | 无公共头变更，本轮无需同步，未声称 SDK 资格 |

## 完整 implementation 文件清单

- `docs/render-v2/00_README.md`
- `docs/render-v2/F0_PROVENANCE.md`
- `docs/render-v2/F0_STAGE_INTERPRETATION.md`
- `docs/render-v2/F0_WORK_ORDER.md`
- `docs/render-v2/final/00_README_权威总纲.md`
- `docs/render-v2/final/01_哲学_原则_完整性.md`
- `docs/render-v2/final/02_分层_类型_所有权.md`
- `docs/render-v2/final/03_通信_数据_事务.md`
- `docs/render-v2/final/04_RenderGraph_作者模型.md`
- `docs/render-v2/final/05_Shader_自动绑定.md`
- `docs/render-v2/final/06_逻辑图编译_优化.md`
- `docs/render-v2/final/07_Vulkan编译_执行_同步.md`
- `docs/render-v2/final/08_Runtime_Scene_View_Target.md`
- `docs/render-v2/final/09_Feature_Capability_插件.md`
- `docs/render-v2/final/10_Legacy功能_业务数据.md`
- `docs/render-v2/final/11_UE级画质_先进能力.md`
- `docs/render-v2/final/12_Cpp20_实现哲学.md`
- `docs/render-v2/final/13_错误_性能_生命周期.md`
- `docs/render-v2/final/14_API范例_全链路.md`
- `docs/render-v2/final/15_实施阶段_工作范围.md`
- `docs/render-v2/final/16_验收与功能矩阵.md`
- `docs/render-v2/final/17_LLM实施合同.md`
- `docs/render-v2/final/18_历史规范迁移.md`
- `docs/render-v2/final/19_冻结决策与歧义消解.md`
- `docs/render-v2/final/FINAL_全部章节合订版_只读.md`
- `docs/render-v2/final/IMPLEMENTATION_STYLE_START_HERE.md`
- `docs/render-v2/final/MANIFEST.sha256`
- `docs/render-v2/final/README.md`
- `docs/render-v2/final/_references_R2_readonly/00_README_总览与文档索引.md`
- `docs/render-v2/final/_references_R2_readonly/01_现状问题与Legacy冻结策略.md`
- `docs/render-v2/final/_references_R2_readonly/02_分层架构与依赖规则.md`
- `docs/render-v2/final/_references_R2_readonly/03_Core与Transport_数据契约和通信.md`
- `docs/render-v2/final/_references_R2_readonly/04_RenderGraph与VulkanFoundation.md`
- `docs/render-v2/final/_references_R2_readonly/05_VulkanBackend_Runtime_Frame_Target.md`
- `docs/render-v2/final/_references_R2_readonly/06_Feature系统与SceneCapability.md`
- `docs/render-v2/final/_references_R2_readonly/07_RenderSystem_Projection_ResourceDomain.md`
- `docs/render-v2/final/_references_R2_readonly/08_Cpp_RAII_错误语义与热路径规范.md`
- `docs/render-v2/final/_references_R2_readonly/09_实施阶段_VerticalSlice_算法迁移.md`
- `docs/render-v2/final/_references_R2_readonly/10_验收门禁与LLM实施合同.md`
- `docs/render-v2/final/_references_R2_readonly/11_R0_实施工作单.md`
- `docs/render-v2/final/_references_R2_readonly/LuxEngine_RenderV2_FinalDocs_R2_SHA256.txt`
- `docs/render-v2/final/appendix/CPP20_讨论稿只读.md`
- `docs/render-v2/final/appendix/EVIDENCE_INPUTS.md`
- `docs/render-v2/final/appendix/GoldenWorkloads.csv`
- `docs/render-v2/final/appendix/LegacyCapabilityMatrix.csv`
- `docs/render-v2/final/appendix/RequirementsTrace.csv`
- `docs/render-v2/final/appendix/TypeOwnership.csv`
- `docs/render-v2/final/tools/check_package.py`

## 交接与停止

F0 不创建 F1 生产代码或为后续阶段自行放行。下一阶段需用户审阅，并以 FINAL 第 15/17/19 章及
阶段解释表生成精确 F1 工作单；其主要入口是 PassSchema/Shader toolchain/typed Builder，禁止提前进入
Native Graph 或 Runtime。后续每阶段仍需独立 I/V、真实适用测试、推送和 STOP。

```text
F0 = PASS (documentation qualification only)
V2_PRODUCT = EXPECTED_UNAVAILABLE
NEXT_STAGE = F1 (await explicit user review/authorization)
STOP
```
