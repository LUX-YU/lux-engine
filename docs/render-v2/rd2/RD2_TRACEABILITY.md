# RD2-D0-C2 逐项追踪与计数规则

原 appendix 四表字节冻结；新增映射是本候选的设计对账，不是另一份可变施工状态账本，不填写新的功能 PASS。

- [要求映射](RD2_REQUIREMENT_MAP.csv)：162 行，逐项涵盖 86 capability、51 requirement、25 workload，保存原阶段、行哈希和迁移说明。
- [旧类型逐项映射](RD2_TYPE_RESPONSIBILITY_MAP.csv)：131 行，保留原候选条目，纠正 Feature-owned 持久数据、history、共享资产、Graph resolution 等责任，不要求创建 131 个新类型。
- [新增目标类型与三个数据类别/四作者样本](RD2_CONSISTENCY_AUDIT.md)：每种类型列实际状态、owner/thread/lifetime/error/阶段。

SourceRowSHA256 使用原 CSV 行的 UTF-8 JSON（sort_keys=True、ensure_ascii=False、标准 separators）计算，只是行身份，不替代原 CSV 字节哈希。原 NewDesignChapter/FinalChapters 仍指原 FINAL；映射列 RD2Chapters 才指新章节。没有章节字段的旧 Workload 由 RD2 第 17/18 章统一保留。

所有原 RequiredAcceptance、SourceFile、OperationSchema、EvidenceRequired 等仍以 appendix 原行全量内容为准。短映射绝不删除长要求。未来能力资格必须引用实际 I/V 与 compile/cpu/gpu/perf/sdk/product 类别；本轮 NewFunctionalEvidence=NONE_DOC_ONLY。

## 新增 DF 门禁的交付责任

| ID | 本阶段设计检查 | 必需后续证据阶段 |
|---|---|---|
| DF01 | CPU/Wire/GPU 约束分开，唯一 generator | F6 ABI/编译/真实 GPU |
| DF02 | Handle owner/generation 与 sparse/dense 分开 | F7 CPU/ASan/churn |
| DF03 | Dirty 与 admission/ready/completion 分开 | F7 GPU/背压/性能 |
| DF04 | 同类型多 key/scope 显式选择 | F6/F7 compile/CPU/GPU |
| DF05 | Feature 不默认拥有集合 | F6/F7 lifecycle/GPU |
| DF06 | 两 owner/两 Scene，结构槽与实例分离 | F6 Shader/GPU/计划缓存 |
| DF07 | GPU authority 不暴露伪 CPU get/patch | F7 GPU/跨帧 |
| DF08 | 2D 不强制 Mesh | F6/F7 小消费者；F10 完整 Sprite/Canvas |
| DF09 | erase/resize/alias 与 last-use | F4-C native风险、F7 resident、后续业务 |
| DF10 | Composition 与 Graph hazards 分层 | F6/F7 CPU/GPU |
| DF11 | 相容计划共享≠无条件共享工作 | F5 Runtime/GPU；F8 Skinning 补证 |
| DF12 | Host-owned metadata 与插件 code pin | F6 installed SDK/GPU |
| DF13 | typed worksets，不编造布局收益 | F7 机制性能，F8–H6 实际算法 |
| DF14 | 开放业务 T、受控 Native primitive | F6/F7 新类型样本，H 阶段实际原语 |
| DF15 | 产品与原缺测分别记录 | F11 产品；历史 IME/平台不回填 PASS |

全部 DF 为 DESIGN_TARGET；D0 不关闭任何 DF 功能行。F11 验收 F01–F36/I01–I44，H01–H06 各在独立 H 阶段关闭。F4 当前 PARTIAL 及 NOT_IMPLEMENTED 原样保留。
