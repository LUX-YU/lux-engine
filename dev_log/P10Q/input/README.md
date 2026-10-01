# P10Q 实施包入口

本包是在 P10 与 P11 之间插入的一次代码质量收敛阶段，未修改任何引擎仓库。

**先读 [00_P10Q_IMPLEMENTATION.md](00_P10Q_IMPLEMENTATION.md)，再读 [01_CONTINUING_RULES.md](01_CONTINUING_RULES.md)。** 主文档是本阶段唯一实施入口，涵盖九项要求、Q0–Q8 顺序、文件/类型迁移与删除、34组验收主题。34组是测试主题，不是34个新程序或模块。

[02_SOURCE_AUDIT.md](02_SOURCE_AUDIT.md) 说明实际读到的代码、最新 P10 R1 交付、lux-cxx API 与待验证项；`reference/` 仅保留原审阅，不能覆盖现行代码；[tools/README.md](tools/README.md) 提供可选的只读盘点脚本用法。

当前参考：lux-engine `b583e7ffe20e7a1ac55c7119d6a13ac337ebb323`；lux-cxx `bc1eab34b83b5cf8821d6319e5b2a02574dc91fd`。开始执行先核对实际 HEAD、祖先关系和用户修改。P10 R1 已有新交付，本阶段核验继承，不重复从零实施，也不据此自动放行 P11。

完成 P10Q 后停下复审。后续 P11–P13 编号不变，均引用统一持续规则。

本包文档/链接/脚本自检不构成引擎、SDK、GPU、IME 或跨平台测试成绩。
