# P10 启动包

P09 R1 的源码与提交证据复审通过；只授权实施 P10，完成后停下复审。

先阅读 [启动补充](LUX_ENGINE_P10_START_AFTER_P09_R1_2026-09-30.md)，再交叉读取 [原 V4 P10](reference/v4/phases/P10_views_and_desktop.md)。后者的正文和配套文件均为原字节历史参考；不得将旧路径、种子账本或脚本覆盖当前工程。

已审阅前置：`3363f83dcc448db9e79cea97c5176a546c05cf5a`；对应实现：`c5bf69a2bb722741391984d5b584d3f90651eaeb`。

启动补充包含 P09 R1 结论、P10 类型/所有权/模块表、A–G 内部实施顺序、逐文件迁出/暂留表、原七项 X10 展开、实际 GPU 与输入要求、交付边界和可直接发送的指令。它不是已经实施的代码或测试报告。

审阅范围见 [REVIEW_SCOPE.json](REVIEW_SCOPE.json)。没有独立引擎、SDK 或 GPU 运行成绩。`PACKAGE_VALIDATION.json` 只记录本包完整性和文档链接检查，不属于 P10 产品验收。

唯一可变施工材料仍为 `.internal/editor-redesign/`；最终在 `dev_log/P10/` 冻结记录。新双视口 GPU 是 P10 必测，不能拿旧两项模式或 mock 替代；不得提前安装第二个 Editor 产品。
