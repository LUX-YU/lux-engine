# LuxEngine Render V2 — FINAL Architecture F1 / 2026-10-10

**唯一权威入口：** [00_README_权威总纲.md](00_README_权威总纲.md)。**实施入口：** [17_LLM实施合同.md](17_LLM实施合同.md)。**整体代码风格与设计哲学：** [12_Cpp20_实现哲学.md](12_Cpp20_实现哲学.md)。**已闭合架构选择：** [19_冻结决策与歧义消解.md](19_冻结决策与歧义消解.md)。

本包包含 20 份按章节编排的最终设计、四份结构化追踪 CSV、可本地验证的 SHA256 清单与检查脚本、合订版只读便览、Legacy 原 R2 文档的只读历史副本。按文件名 `00` → `19` 阅读即可，不需要从聊天记录拼凑规则。

**本包是可实施的“最终目标规范”，不是新的 GPU 测试报告，也不是直接修改 GitHub 后的代码。** R0–R4 原历史验收事实不变，`F1` 等后续工作会按第 15/17 章分阶段各自新增证据。Implementation agent 启动时先从 HEAD `7de3ddaa3f7614995745b41d73dfc98302310a4c` 审查实际远端，先执行 F0 doc-only check-in，独立验收后 STOP。

## 目标硬约束概览

- 完整保留所有 Legacy 功能，不能以最小 Graph/Renderer/Feature 代替完整迁移；66 项原始能力行均有 source/acceptance，新技术行另计。
- 一 Runtime/Backend，N Scene/N View/N Targets；**View 不是 Graph 默认 owner**；同渲染路径共享 plan，Scene-global GPU 工作有证明才共享，Multiview 可选优化。
- typed PassParams 统一 Graph/Shader 访问；C++20 values/free algorithms/Builders/RAII/concepts，外部插件不包含私有 Vulkan/RenderScene 头。
- Engine PROGRAM/CONTROL/UPLOAD 三 Lane 不变，O(changed) 发布、source revision/backpressure 保真，Frame progress 独立 Simulation，GPU retirement fence-proven。
- Vulkan Foundation 复用当前 R4；Graph native compiler 必须真实处理 Shader LayoutPlan、Graphics/Compute/Transfer、subresources、queues/barriers、aliasing、record/present/readback。
- UE 级质量目标是明确算法与图像性能交付，不是用 UE 命名已有 Pass。
- 每阶段的 Type Inventory / compile negatives / clean clone I/V / GPU/SDK scope / STOP 必须执行。

## 附录与验证

| 文件 | 内容 |
| --- | --- |
| `appendix/LegacyCapabilityMatrix.csv` | 86 行：66 项历史 F/I + 14 项新执行要求 I31–I44 + 6 项高级 H01–H06；逐项迁移/验收 |
| `appendix/TypeOwnership.csv` | 131 个最终值/算法/Builder/Owner/Plugin types 或接口职责，标注作用域/阶段 |
| `appendix/RequirementsTrace.csv` | 51 条硬性要求 → 章节 → Workload → 阶段 |
| `appendix/GoldenWorkloads.csv` | 25 条 CPU/Shader/GPU/运行时/插件/画质测试工作负载 |
| `appendix/EVIDENCE_INPUTS.md` | 当前事实 SHA、源码与官方 Vulkan/UE 技术入口；不冒充本轮测试 |
| `appendix/CPP20_讨论稿只读.md` | 历史讨论稿，**正式权威版本是第 12 章** |
| `_references_R2_readonly/` | 用户原始 R2 设计 00–11 的历史只读内容，**当前不能覆盖 Final** |
| `FINAL_全部章节合订版_只读.md` | 为阅读生成的派生合订版；修改时请修改各章节再重新生成，**不独立设定权威** |
| `MANIFEST.sha256` 与 `tools/check_package.py` | 校验文件完整性、章节/矩阵计数与 Markdown 结构，不是代码/GPU qualification |

可用命令：`python tools/check_package.py`（在任意工作目录运行，Python 3 标准库，无额外依赖）。

**正确执行顺序：F0 文档集成 → 用户审阅 → F1 Shader/Graph authoring → F2 完整 Logical Graph → F3 Native Layout/PSO → F4 GPU Graph → F5 Runtime → F6 Feature SDK → F7 Scene/Data → F8–F11 全部 Legacy Parity → H1–H6 高画质算法。** 每阶段自己的独立 STOP 门禁是为防止实施 LLM 偷工减料，不是授权其跳过后续用户审查。
