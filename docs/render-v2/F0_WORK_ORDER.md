# F0 — FINAL 规范入库工作单

## 授权、基线与范围

用户已明确批准执行新的实施计划，本轮仅执行 F0 文档入库与独立资格验收。
附件中关于未来阶段的指令是设计输入，不构成 F1–H6 的连续施工授权。

- Repository：`https://github.com/LUX-YU/lux-engine`；分支：`codex/render-v2`。
- 实施父提交：`7de3ddaa3f7614995745b41d73dfc98302310a4c`。
- R4 implementation：`83ffbb6d9859d87ca62a8a2acbb0083c2a8de420`。
- Frozen V1：`a669409a289a6fa4092f21176397795b1cdb7f3e`。
- 工作区：既有独立 Render V2 worktree；原用户工作区及 V1 oracle 均不用于施工。
- 包名：`LuxEngine_RenderV2_FINAL_Architecture_2026-10-10.zip`。
- ZIP SHA-256：`39be1dd08ca4e30964ef47fa0b274054bc426e79472a1d0e733deaa0fe83d551`。

| 权限 | 精确范围 |
| --- | --- |
| ALLOWED | `docs/render-v2/final/**`，原包完整内容按字节导入 |
| ALLOWED | `docs/render-v2/00_README.md`，仅前置权威入口与 supersession 说明 |
| ALLOWED | `docs/render-v2/F0_*`，工作单、解释、来源保护和独立验收记录 |
| READ-ONLY | 全部生产源码、现有测试、历史设计和验收报告、Legacy、用户六处修改 |
| FORBIDDEN | Root/product/bootstrap CMake、生成器实现、第三方仓库、任何生产 C++ 或后续阶段代码 |

## 交付物及唯一权威

完整保存包内 44 个文件：43 个被哈希的文件和 `MANIFEST.sha256`，包括 20 章、四张 CSV、
README、只读合订版、历史引用和包校验脚本。原包文件不新增内容、不改编码或换行。
RD0 草案及 66 行矩阵仅作为外部归档输入，不再另存为仓库内施工规范。

权威阅读入口为 [final/README.md](final/README.md)，执行入口为
[17_LLM实施合同.md](final/17_LLM实施合同.md)。冻结决策按第 19 章，阶段与权限按第 15/17 章；
C++20/RAII 按第 12/13 章，实际排版按仓库 AGENTS.md。
[阶段解释表](F0_STAGE_INTERPRETATION.md)落实用户已批准的跨阶段解释，不替换或删减规范要求。

本轮新增生产类型、算法、CMake target、测试 target 和公共 API 的集合全部为空。
131 项 TypeOwnership 是后续责任审查清单，不是本轮创建类型的任务。
86 项能力、51 项要求和 25 项 Workload 的证据字段保持包内初始值。
未来报告按原 ID 追加真实证据；没有齐备证据时不得关闭整个能力行。

## 实施与证据保护

1. 核验实际远端及本地 HEAD，V2 worktree 干净；不 reset、stash、强制推送或复用无关 worktree。
2. 源码树外保存原 ZIP、manifest、两个工作区 HEAD/status/diff/untracked、六文件原字节与哈希、完整 Git tree。
3. 原样导入；旧 README 只前置说明，原正文保持完全一致的后缀。历史文档删除集合为空。
4. 保存历史设计到 FINAL 的映射，特别核对现有五份 R2-FIX 架构修订。
5. 检查允许路径、原包完整性、来源与保护；提交文档 implementation I。
6. 执行 tracked snapshot 检查，从 I 建立 `clone --no-hardlinks --no-checkout` 的独立 clean clone。
7. 独立检出执行文档资格，归档命令、退出码、脚本、检查结果和 evidence SHA-256 manifest。
8. V 只新增 `F0_VERIFICATION.md`。发现缺陷先产生新 I，再从新 I 重新验收。
9. 核验 V-only diff、原工作区保护、远端未被其他提交推进；普通 fast-forward push 后核验远端 SHA，STOP。

包内 Markdown 使用 LF、四张 CSV 使用 CRLF。V2 worktree 的 `core.autocrlf=false` 是本工作区的 Git
元数据设置；原工作区的配置不改变。独立 clone 必须在 checkout 前设置 `core.autocrlf=false`，
并同时验证 Git blob 与检出字节，不能以“文本等价”代替原包 SHA 一致。

## 独立验收门禁

| 检查 | 必须满足 |
| --- | --- |
| 来源身份 | 父提交、分支、ZIP SHA、43 条 manifest 及包内完整覆盖一致 |
| 文档结构 | 20 章、成对代码围栏、Markdown 本地链接、历史引用完整 |
| 四张 CSV | 86/131/51/25 行，唯一主键、章节/Workload 引用合法，初始状态和证据列未改 |
| 历史来源 | 66 个旧能力编号保留，102 处源码/operation 引用匹配冻结提交 |
| Legacy | 719 个源文件 mode/blob/size 精确匹配；含保护 CMake 的 archive tree 共 720 项 |
| 源码保护 | 所有允许范围外的 tracked tree 完全不变，Core/Transport/Graph/Vulkan 分别核对 |
| 历史事实 | 所有既有报告和已知问题未改；五份架构补丁的约束在 FINAL 中有对应位置 |
| 用户保护 | 原工作区 HEAD/branch/status/diff/untracked 与六文件哈希保持开工值 |
| 提交纪律 | I 文档范围合规；验收 clone clean 且绑定 I；V 仅新增报告 |

F0 不配置或构建 bootstrap，不运行 CTest、GPU、Sanitizer、性能、产品、SDK、Linux 或 Android 矩阵；
这些均为本阶段 `NOT_RUN`，不引用历史 59/59 或 ASan 13/13 充当新测试。
没有公共头变更，三个 install include 前缀无需同步。

## 后续路线与停止

[F1–F11/H1–H6](final/15_实施阶段_工作范围.md)依序单独授权：Graph/Shader authoring → logical graph →
Layout/PSO → native graph → Runtime → Feature SDK → Scene publication → Mesh → Lighting →
2D/Terrain/PointCloud/UI → 产品与全量 parity → 六个先进算法阶段。
F11 关闭 F01–F36/I01–I44，H01–H06 在各自算法阶段独立关闭；不得提前缩减功能或改变画质换取 PASS。
每阶段须重新冻结精确修改范围、源码算法来源、消费者、真实 oracle、类型责任、I/V 与 STOP。

```text
F0 PASS = documentation integration only
V2_PRODUCT = EXPECTED_UNAVAILABLE (until F11)
NEXT_STAGE = F1 (requires explicit user review/authorization)
STOP
```
