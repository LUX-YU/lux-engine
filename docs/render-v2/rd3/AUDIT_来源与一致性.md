# RD3 来源、重写边界与静态一致性审计

## 基线

- 原始用户上传：`RenderV2_RD2_Architecture_and_Implementation(2).zip`；提取有 34 项：21 个独立章节、5 张工作单、4 个 CSV 附录、README、合订版、来源审计和 SHA Manifest。
- 用户交接：`RenderV2_RD2_Architecture_and_Implementation_HANDOFF_2026-10-11(3).md`，属于 2026-10-11 F5 时点快照，**不再是 2026-10-12 的最新进度权威**。
- 本次对照 Git：`LUX-YU/lux-engine` `codex/render-v2` HEAD `246fe21eb6d060801b9a16ddcadde2be89dbbecc`；F6.b I `7406965112f09807b9f70aa3e739ff43539969c8`，V `246fe21eb6d060801b9a16ddcadde2be89dbbecc`。
- Frozen Legacy SHA：`a669409a289a6fa4092f21176397795b1cdb7f3e`，719 源文件只用于真实映射，未由本 ZIP 修改。

## 这次真正重写的内容

- 21 章全部改写为 RenderEffect→GraphIR→Native Graph/Executor + Scene/View/Target/ResourceDomain 的新模型；旧 Scope DataSet 作为强制 CPU authority、Scene owns Feature/Graph、Runtime 直接包含 Backend Native public API 等不再作为正式设计。
- 全部新阶段工作单重写为 RD3-D0/P1–P5/Legacy；历史 F4/F5/F6.a/b 只作为真实已实施内容/资格记录。
- 四份 CSV 全部重构字段与新 owner 映射：CapabilityMatrix 86 项，GoldenWorkloads 25 项，RequirementsTrace 51 项，TypeOwnership 为新类型清单。原 SourceFile/历史操作与 Oracle 仅作可追溯事实，不能把旧名误用为新 API。
- 合订版从新章节自动生成；SHA Manifest 由此包所有文件内容计算；对 Markdown 代码围栏、章节覆盖、需求编号、矩阵外键和 ZIP CRC 进行程序检查。

## 严格限制

这是**静态架构/文档产物**；没有修改仓库源文件、没有在用户 Windows 环境运行 MSVC/Full ASan/Vulkan GPU，亦没有声称正式独立 I/V 已通过。原 `F6B_VERIFICATION.md` 报告的 GPU/性能结论是报告来源，不是本次复测。文档包获用户批准后，需在独立的 Docs-only Gate 提交、验证、STOP；F6.c 仍无授权。

## 本包机械检查结果

- 独立核心章节：21；Work Orders：7；新 TypeOwnership 记录：60。
- Capability ids：86（F36 + I44 + H6）；Golden Workloads：25；Requirements：51。
- 章节合订从新正文生成，代码 fenced Markdown 各自配对；CSV 读取可验证所有编号没有遗漏/重复。
- 本检查**不是** GPU/Build/ASan/Git independent verification。
