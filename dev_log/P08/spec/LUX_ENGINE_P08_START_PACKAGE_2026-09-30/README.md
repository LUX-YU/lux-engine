# P08 启动包

先读 [START.md](START.md)。它包含 P07 R1 复审、P08 范围、类型关系、内部顺序和可直接发送的实施指令。

原始 [P08](reference/v4/phases/P08_interaction_and_detached_ui.md) 及其配套索引位于 `reference/v4/`，从原 V4 ZIP 逐字复制，仅是历史参考。不得用旧路径、种子账本或旧审计脚本覆盖当前已实施仓库；继续使用现行 `.internal/editor-redesign/`，阶段末冻结到 `dev_log/P08/`。

本次前置：`833d7efb18f8349cda9968ac3e4ea24ff2b54c60`。只授权 P08，不授权 P09，也不修改 main。

[审阅范围](review/REVIEW_SCOPE.json) 明确记录：本次没有执行引擎、SDK、GPU、编译负例或完整归档验证，没有新增独立运行成绩。`PACKAGE_VALIDATION.json` 与 `SHA256SUMS.txt` 只验证本包文件，不是引擎验收结果。
