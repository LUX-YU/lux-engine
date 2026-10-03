# EC2 施工包

**下一阶段：职责重分配 + 游戏脚本能力。** 新代码基线为 `248adc4576943cab83976afd8d1d5f31b63b70a9`。

先交给实施 LLM [00_MASTER.md](00_MASTER.md)。需要一份完整文件时使用 [FULL_EC2_CONSTRUCTION.md](FULL_EC2_CONSTRUCTION.md)。

| 文件 | 内容 |
|---|---|
| [01_BASELINE_AND_ARCHITECTURE.md](01_BASELINE_AND_ARCHITECTURE.md) | EC1 已完成/仍需迁移/新脚本范围与层级 |
| [02_RESPONSIBILITY_IMPLEMENTATION.md](02_RESPONSIBILITY_IMPLEMENTATION.md) | 发布、配置、活动、面板、预览、Workspace、模型 recipe |
| [03_GAME_SCRIPT_CAPABILITIES.md](03_GAME_SCRIPT_CAPABILITIES.md) | 原生复用、ScriptAbility、Lua、句柄/作用域/错误/异步完整契约 |
| [04_WORK_PACKAGES_AND_FILES.md](04_WORK_PACKAGES_AND_FILES.md) | R0–R9 依赖顺序与具体文件/类型处置 |
| [05_ACCEPTANCE.md](05_ACCEPTANCE.md) | 38 个行为主题、负例与实际验证范围 |
| [06_CONTINUING_RULES_AND_REPORTING.md](06_CONTINUING_RULES_AND_REPORTING.md) | 架构判据、AGENTS、ABI、交付规则 |
| [07_SOURCE_INDEX.md](07_SOURCE_INDEX.md) | 固定来源、读取范围及证据边界 |
| [IMPLEMENTATION_MAP.json](IMPLEMENTATION_MAP.json) | 施工种子，不是完成记录 |
| [references/AGENTS.user.md](references/AGENTS.user.md) | 用户原规范字节副本，当前规则按施工仓库再核对 |
| [references/PRIOR_RESPONSIBILITY_AUDIT_54f8a563.md](references/PRIOR_RESPONSIBILITY_AUDIT_54f8a563.md) | 上次调查原字节；旧基线，不直接当当前未修清单 |

本包不含新生产代码或引擎运行证明。EC1 的完成声明与本次定向源码核对分开；本包不自动给 EC1 作全面验收。
