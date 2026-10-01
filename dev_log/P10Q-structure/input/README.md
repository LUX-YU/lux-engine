# Editor 五层收敛：LLM施工包

**使用方式：** 可以将整个ZIP交给实施LLM，并要求先读 `00_MASTER.md`。需要一份文件时使用 `FULL_CONSTRUCTION.md`；它包含全部施工正文。

只实施P10Q-structure，按L0–L6顺序闭合，最后复审；不进入P11。原stage仍P10Q，不重编号。用户已取消旧慢算法补满和当前Linux实测硬阻塞，这两项决定必须继承。

## 文件导航

| 文件 | 用途 |
|---|---|
| [00_MASTER.md](00_MASTER.md) | 总授权、基线、边界、执行与停止规则 |
| [01_DECISIONS_AND_INVARIANTS.md](01_DECISIONS_AND_INVARIANTS.md) | D01–D07裁定与I01–I18继承不变量 |
| [02_PATH_TYPE_TARGET_MAP.md](02_PATH_TYPE_TARGET_MAP.md) | 71条路径种子、类型与target处置、非Editor调用者 |
| [03_CONCEPTS_AND_DYNAMIC_BINDING.md](03_CONCEPTS_AND_DYNAMIC_BINDING.md) | 实际共享交付算法、概念语义、动态边界 |
| [04_BUILD_AND_DEPENDENCY.md](04_BUILD_AND_DEPENDENCY.md) | CMake、头、模板、链接、安装及14种负例 |
| [05_ACCEPTANCE.md](05_ACCEPTANCE.md) | 24组观察主题，不是新测试数量配额 |
| [06_FOLLOWING_PHASES.md](06_FOLLOWING_PHASES.md) | P11/P12/P13的持续规则 |
| [07_RECEIPT_AND_RESUME.md](07_RECEIPT_AND_RESUME.md) | 唯一账本、接力、证据与最终摘要 |

## 顺序施工文件

| 批次 | 文件 |
|---|---|
| L0 | [基线和逐文件计划](phases/L0_BASELINE.md) |
| L1 | [editing与authoring](phases/L1_EDITING_AUTHORING.md) |
| L2 | [activities](phases/L2_ACTIVITIES.md) |
| L3 | [workbench](phases/L3_WORKBENCH.md) |
| L4 | [application与旧产品](phases/L4_APPLICATION.md) |
| L5 | [concept与动态绑定](phases/L5_ABSTRACTIONS.md) |
| L6 | [最终资格](phases/L6_QUALIFICATION.md) |

每批次使用总指令、裁定、路径表及该批文件。references为原设计，不是第二份施工账本；本包D01明确纠正了原稿对ProjectBuilder的推断，它当前只是纯配置Builder。

## 辅助工具

`scripts/plan_inventory.py`只从实际Git树生成逐文件**待审核**计划，使用`templates/path-rules.json`。不读完整源码做语义推断，不移动、不删除、不暂存、不fetch，不替架构检查器宣布PASS。未知文件、混合文件和目标冲突都需要L0裁定。

模板仅定义必备字段，合并入既有`.internal/editor-redesign/`；不新建重复管理系统。

## 验证范围

本包未修改lux-engine仓库，未运行引擎/SDK/GPU。脚本在合成Git仓库做安全性自检；C++示例仅为合成Result/actions。详见 [来源和边界](appendix/SOURCES_AND_LIMITS.md) 以及 `evidence/package-selfcheck.json`。
