# EC3 施工包

本包是下一阶段实施规范，不是引擎修改或验收结果。

固定输入：`126f1b4df14316df30957208787ded9be9f9f461`。实施前核对实际后继，不回退用户工作区。

## 先读

[完整施工总册](FULL_EC3_CONSTRUCTION.md)；[总指令](00_MASTER.md)；[逐项问题登记](01_ISSUE_REGISTER.md)；[C0–C9 顺序](08_EXECUTION_SEQUENCE.md)。

## 关键分册

[身份与同型描述](02_IDENTITIES_AND_DECLARATIONS.md)说明哈希边界、原 handle、constexpr、动态存储和菜单寿命；不再采用 CommandSpec→拥有型 Descriptor 的双份方案。

[Application 与命令](03_APPLICATION_AND_COMMANDS.md)说明项目保存／工作区迁出、模块声明、准确 receiver 和关闭排空。

[设置与启动](04_SETTINGS_AND_STARTUP.md)说明作用域、动态贡献、真实生效、窗口／显示器／字体与快捷键。

[相机与封装](05_CAMERA_AND_ABSTRACTION.md)保留原 ECS 链，逐边辨别冗余，核查显示与拾取。

[代码生成](06_CODEGEN_UNIFICATION.md)规定原 MetaUnit/语义/inja、作者／Run 明确投影、增量与安装闭包。

[质量与计量](07_QUALITY_AND_MEASUREMENT.md)、[验收主题](09_VALIDATION.md)、[来源与裁定](10_SOURCES_AND_DECISIONS.md)。

## 规模与口径

本包有 **149 个逐项约束/问题**，覆盖已确认问题、新设计要求、保留契约和待验证风险；不是宣称有 149 个已复现故障。

验收有 **60 个主题**，不是新增 60 个测试程序的配额。

初始声明清单继承原调查：36 条定义路径、35 个固定命令 ID、12 个 view 描述、3 个 source 描述组。动态插件 family 单独处理。C0 需要展开完整实际消费者，不将原清单冒充全仓统计。

## 使用清单

`inventory/issues.csv/json` 是输入追踪；`decl_commands.csv`、`decl_views.csv`、`decl_sources.csv` 给出逐声明目标；`file_actions_seed.csv` 是40条路径处置种子，不是自动移动或删除脚本；`validation_topics.json` 与验收分册一致。

`templates/` 是未执行的账本/测量/收据结构，必须填写实际数据；不可把模板里的空值当作资格。原始资料在 `reference/`，不改写它们的历史含义。

## 状态边界

唯一可变施工材料仍在仓库 `.internal/editor-redesign/`。EC2 原生输入延期、P12 免验、Linux／IME 未测及旧性能 PARTIAL 保留。EC3 新功能单独验证，不能扩写旧通过范围。

没有修改仓库、main、实施分支或用户补丁；没有执行 Editor、SDK、GPU、窗口、codegen 或新性能资格。

本包自检只证明文档/清单/链接和归档自洽。实现与验收分别提交，完成后停在 EC3；合并/删分支/发布另需用户授权。
