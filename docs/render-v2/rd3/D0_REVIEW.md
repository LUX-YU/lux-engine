# RD3-D0 实施审查入口 — PARTIAL，等待用户审查

本目录于 2026-10-12 按用户明确指令实施 **RD3-D0 docs-only**。输入包内的
`DRAFT / NOT_AUTHORIZED` 是包产生时的状态；当前授权仅覆盖 D0 的文档审查、I/V 提交和推送。
P1–P5、L1–L3、产品切换和 H1–H6 均未授权，本文也不授予授权。

远端查询和施工基线均为 `246fe21eb6d060801b9a16ddcadde2be89dbbecc`（F6.b V），
真实 parent 为 F6.b I `7406965112f09807b9f70aa3e739ff43539969c8`。
本轮使用独立 managed worktree；用户原工作区仍在原 HEAD，保留 `.vscode/settings.json` 修改。
历史包的“七个 dirty 文件”不是当前工作区事实；本次单独记录当前状态及七个本地配置/辅助文件哈希。

## 阅读顺序和权威边界

1. [架构审查、最小反例和未决建议](D0_ARCHITECTURE_REVIEW.md)。核心架构可成立，但完整冻结门禁有未决项。
2. [逐项追踪补充](D0_TRACE_REVIEW.csv)：86 能力 + 25 workload + 51 requirement，共 162 行。
   原四表完整保留，建议阶段和责任补充不冒充已批准裁决。
3. [现有源码迁移映射](D0_MIGRATION_MAP.csv)、[60 项类型审查](D0_TYPE_REVIEW.csv)、
   [源码/历史 Git 锚点](D0_SOURCE_ANCHORS.json)。这些是迁移责任及未来 oracle，不是生产实现。
4. [来源与历史资格](D0_PROVENANCE.md)、[P1 入场建议](D0_P1_ENTRY.md)。
5. 独立验收提交只新增 `D0_VERIFICATION.md`；I 的确切 SHA 由该报告记录，V SHA 由 Git 取得。

原 ZIP 的 38 个文件（21 章、LAYOUT、7 工作单、4 CSV、合订版、来源/交接及 Manifest）在此逐字节保留。
`MANIFEST.sha256` 仍为输入包的清单。新增 `D0_MANIFEST.sha256` 覆盖本轮全部 I 文档和检查脚本，
只排除自身及未来 V 报告，避免递归哈希。输入正文与本轮审查必须一起阅读；未决建议不静默覆盖正文。

**未修改任何旧 RD2 权威指针。** RD3 是已入库并被审查的候选；正式替换旧设计权威、解决本轮未决项，
仍等待用户审查。旧 RD2/FINAL、历史 I/V、性能和业务资格保持原样。

## 目录权限

| 边界 | 本轮范围 |
|---|---|
| ALLOWED | 仅新增 `docs/render-v2/rd3/**`：原包、审查/迁移/追踪、文档专用校验材料；V 仅新增验收报告 |
| READ-ONLY | 旧 RD2/FINAL、所有历史报告、输入附件、Legacy 719 文件、生产/测试源码、依赖仓库、用户工作区 |
| FORBIDDEN | 生产 C++、Vulkan/Graph 实现、生产 CMake/构建脚本、已有测试修改或删除；main 合并；P1 施工；改判历史 PARTIAL |

检查脚本在 `checks/verify_d0.py`，只读源码并把证据写到显式指定的源码树外目录；不是生产构建入口。
本次不构建或运行 GPU/ASan，不声称新架构或产品功能通过。实现文件全集由 Git I 的 add-only diff 和 D0 Manifest 列出。

## 当前结论

输入完整、历史保护、ID/原始需求保留可以独立机械验证。**设计冻结结论 PARTIAL**：
共享 Target 的多 Effect 组合及其更新事务闭包未充分定义；CSV 阶段与工作单不完全一致；类型表的具体回滚/责任仍需审查补充。
细节、建议和关闭条件在架构审查中逐项保留。不是以数量检查替代语义验收，也不是把历史性能债转为 D0 新 PASS。

全部高级方向仍是未来真实算法目标。当前 D0 不完成 F01–F36、H01–H06 或产品切换。
推送本轮 I/V 后 STOP，等待用户审查；不得自动进入 P1。
