# P09 启动包

先读 [LUX_ENGINE_P09_START_AFTER_P08_R1_2026-09-30.md](LUX_ENGINE_P09_START_AFTER_P08_R1_2026-09-30.md)，再读 [原 P09](reference/v4/phases/P09_layout_storage.md)。

本包授权范围为：从 P08 R1 验收 `41167a9bdff8c192fe990d53aa8dfa132d57b081` 继续 P09，阶段末停下复审，不进入 P10。

`reference/v4/` 是原 V4 的逐字历史参考，不能覆盖当前工程的路径、施工账本、阶段门禁或实际 API。唯一可变施工材料仍在仓库的 `.internal/editor-redesign/`；`dev_log/P09/` 只存最终冻结验收。

本次没有独立运行引擎、SDK 或 GPU，也没有新增探针成绩。源码/证据审阅范围见 [review_scope.json](review_scope.json)。本地文件完整性检查见 [PACKAGE_VALIDATION.json](PACKAGE_VALIDATION.json)。

特别继承本轮证据校正：GPU 模式须有明确 CONSUMER_MODE、实际配置与 test command；消费者目录名不能作为 GPU 已执行证据。
