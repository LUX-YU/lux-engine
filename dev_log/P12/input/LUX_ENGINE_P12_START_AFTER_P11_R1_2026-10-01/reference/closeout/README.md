# P11 / P12 更新施工包

**执行入口：[总指令](00_MASTER.md)**。默认先做 P11，结束后停下复审；P12 随后完成完整产品与实际删除。

## 正文

- [P11：命令、贡献、工厂与外部 SDK](01_P11_IMPLEMENTATION.md)
- [P12：产品用例、唯一入口和旧体系清零](02_P12_IMPLEMENTATION.md)
- [精确分类的残留移除计划](03_REMOVAL_PLAN.md)
- [验收与交接](04_ACCEPTANCE_AND_HANDOFF.md)
- [完整单文件总册](FULL_P11_P12_CONSTRUCTION.md)

## 辅助资料

[来源索引](reference/SOURCES.md) 区分当前已读源码、原阶段规范和新的施工裁定。reference 内原文件按字节保留，优先级低于本包明确更新。

[只读盘点工具](tools/README.md) 不执行删除。templates 是施工种子，字段未填不构成验收；不得把空 consumers 当成已证明无消费者。

本包没有修改仓库。工具只在合成 Git 仓库进行了自身检查，没有独立重跑 Lux 的 CTest、SDK、GPU 或插件。
