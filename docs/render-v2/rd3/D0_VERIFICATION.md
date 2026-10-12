# RD3-D0 独立 clean-clone 验收 — PARTIAL

日期：2026-10-12。**机械完整性 PASS，设计冻结 PARTIAL。** 原始设计完整入库、迁移与需求审查完成，
但 R01–R04 尚未得到一致的冻结裁决，不能将数量检查当作架构整体 PASS。未执行 P1。

## 精确 Git 身份及独立性

- 查询到的远端和施工基线：`246fe21eb6d060801b9a16ddcadde2be89dbbecc`。
- **Implementation I：`43ba969f4d9ebfa2fe8d39dab833e6c0e1db4103`**，parent 为上述基线。
- F6.b I/V 的真实 parent 关系已检查：`7406965112f09807b9f70aa3e739ff43539969c8` → `246fe21...`。
- 本 Verification V 是 I 的 report-only 子提交，只新增当前文件。V 的确切 SHA 在提交后由 Git 和外部 delivery.json 记录。
- I 新增 48 个文件、4723 行，全部在 `docs/render-v2/rd3/**`；无修改、删除、重命名既有文件。

施工 clean tracked snapshot 通过后，使用 `git clone --no-hardlinks --no-checkout` 新建独立克隆，
固定 `core.autocrlf=false`，detached checkout 精确 I。验证源路径：
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/RD3-D0/20261012/qualification/source`。
该 clone 无 alternates，5147 个 Git object 文件最大链接数 1；Git dir 与 common dir 均位于该 clone。
前后 tracked snapshot/status 均 clean，实际工作文件字节逐个等于 I 的 Git Blob。
本轮“独立”是 Git/文件/执行进程隔离，由同一执行代理完成，不声称另一个人或代理进行了审查。

## 已执行的门禁

| 检查 | 结果 |
|---|---|
| 冻结 I 上的只读文档检查器 | **3460/3460 PASS**；exit 0；results/checks.json 与 result.json 保留 |
| ZIP 安全路径、CRC、SHA、完整入库 | 38/38 文件逐字节相同；输入 Manifest 37/37 SHA 正确 |
| 正文章节、工作单、LAYOUT、合订版 | 21/21 章，7/7 单；合订包含全部章节；另附文件与包内版本字节一致 |
| 四张 CSV | 主键完整无重复；F36/I44/H6 = 86、W25、REQ51、Type60；Golden/章节引用正确 |
| 与 RD2 逐项对比 | 原能力、源文件/操作、验收文字/证据类型、需求文字及 Golden oracle 没有删改 |
| 新补充追踪 | 162 行逐项 Owner/阶段/验收建议；30 条源码责任迁移；60 项类型形式/生命周期/回滚审查 |
| 源码与历史锚点 | 128 个实际路径的 Blob、SHA-256、size；迁移表符号/行号均与施工基线相符 |
| 文档内部引用、围栏、UTF-8 | 检查通过；未把伪代码当成已安装 SDK；未检索外部网页内容 |
| I 的 D0 Manifest | 覆盖 47 项，排除自身及未来 V；原输入 Manifest 未改 |
| 全部既有 tracked 文件 | 基线 3265 项 path/mode/blob/size 不变，包括旧 RD2/FINAL、历史报告、生产和测试 |
| Legacy 冻结 | 719/719 与 `a669409a289a6fa4092f21176397795b1cdb7f3e` 原路径和当前冻结副本一致 |
| 用户工作区 | 原 HEAD/status/完整 binary diff 不变；七个已记录本地配置/辅助文件 SHA 不变 |
| 设计冻结一致性 | **PARTIAL**：R01–R04 未关闭；R05 为待采纳的停止次序说明 |
| 生产/GPU/ASan/性能/安装/Android/Linux | **NOT_RUN**，本轮 docs-only；未使用历史结果冒充本轮执行 |

需求表 REQ08/40/41/50 的 evidence 引用本来就是 R2/全工作负载/文档证据，不是 W 主键；
检查器显式按原值核对这些例外，没有凭空增加假 Golden ID。
追踪补充中建议阶段不是已批准替换，也不把“有 Owner 字段”当作真实功能已实现。

## 类型所有权、模块依赖和设计未决项

核心模型方向一致：Client 与 Server 分离；Backend owns Scene/Target/Effect/Graph/Domain；Scene 真正 owns View；
Effect 配置/Feature 算法与不可变 Graph 分离；Executor owns Context/slots；Native code 共享，执行资源独立。
中立模块不依赖 Vulkan；Native 后端依赖中立层；Engine/Editor 使用 Client。没有需要新 Manager 的证据。
这只是目标架构审查，真实 include/link/install 闭包留在对应获授权阶段验证。

| 项目 | 仍需解决的合同及最小关闭条件 |
|---|---|
| R01 | 同 Target 两个 View 选不同 Effect 时，完整 Graph 的构建输入/身份及 Effect 更新依赖闭包未定义；明确组合输入并增加整体回滚 oracle |
| R02 | 仅 expected Effect revision 不能覆盖候选准备期间 membership/Target epoch 变化；冻结串行化或发布点重验规则 |
| R03 | REQ09/10 的 P3 与 P4 工作单冲突，REQ27–31/51 阶段粒度不足，W/REQ 缺直接 Owner；审查补充表并统一正文/工作单 |
| R04 | 原类型清单缺具体失败回滚/为何需要 class，Native backing 实际唯一 owner 需落实；60 项补充清单待审 |
| R05 | 第 02.3 章停止次序不能导致 Client 解引用已销毁 Transport；保留原 R2 Reply arena 晚析构安全，作为 P1 明确负例 |

源码位置、最小反例、建议和逐章结论见 [架构审查](D0_ARCHITECTURE_REVIEW.md)。R01/R02 是目标合同缺口，
没有冒称本轮已复现当前生产 bug。没有通过新增管理层绕开，也没有在 V 修复 I 的文档。

F01–F36 的 GPU-driven、Skinning、Canvas、PointCloud 五算法、Terrain、Water、Robotics、Picking 等全部保留。
Plugin、Offscreen、History、多 View/Target 及 I01–I44 未减项。H01–H06 的 Nanite-like、GI、TSR、VSM、
高级材质/体积、真实 3DGS 仍需未来 GPU/质量/性能独立验收；SoftSplat 不等于完整 3DGS，4D 仍是后续目标。

## 实际失败、环境修正和可验证限制

1. 实施前检查器曾把伪代码 `[](EffectDraft& x)` 识别为 Markdown 链接，出现两个误报。
   在 I 冻结前修正为忽略 fenced/inline code，并检查围栏闭合；首次失败 JSON 保留。独立 clone 未修改检查器。
2. 独立首次运行 exit 1：Windows 深路径下 Git `show SHA:path` 报 `Filename too long`。
   `06-document-gate.log` 原样保留；只设置该 clone 的 `core.longpaths=true`，随后相同 I、相同命令和输入
   重跑通过，结果见 `09-document-gate.log`。没有修复源码或调整验收标准。
3. 原包包含 CRLF；为了保持附件原字节，未全局转换换行。Git diff whitespace 检查识别 CRLF 后通过
   （临时 `-c core.whitespace=cr-at-eol`）；默认检查曾把 CR 行尾报为 trailing whitespace。用户 Git 配置未改。
4. 历史原始性能目录在本机未找到，详见 [来源记录](D0_PROVENANCE.md)。只能证明 Git 报告和源码未损坏，
   **历史原始样本逐文件重验 NOT_RUN**，不能宣称旧档案全 SHA PASS。
5. 前序本机开发环境 CTest 192/200、8 项 GPU/Runtime 失败，不是本次独立 D0 的执行结果。
   SDK 与历史环境不同，不能据此认定根因。P1 前应建立当前可复现基线，本轮未修生产代码。

历史状态不改判：F4 cold PARTIAL；原 F5 Record/Submit PARTIAL；后续 F5-C p95 CI 上界 +5.5749%
仍 PARTIAL；F5-C-D 尾部 INCONCLUSIVE；F6.b 功能限定 PASS 但总体 PARTIAL，p95 CI 上界 +17.8182%，
cold layout 600→1700 ns、10→24 allocations 的新增成本仍保留。
本次不构成历史性能豁免，也不使产品或高级算法通过。

## Manifest 与外部证据

- 原 ZIP SHA-256：`3ac411f5c6d000b1a7b7a3ffd0e8e3e0b0aeb45936a91433065f9cbc7ead88af`。
- 原合订版 SHA-256：`401aa86b5ba881548c993f0ea8c23311e91227b24d4402cf26a756fb66abf459`。
- 原输入 Manifest SHA-256：`ec5ccf605347e07d4a38ce94d049d3a18466b92cc5e3d1316f9fc4707aed3468`。
- I 的 D0 Manifest SHA-256：`9bda248a0c01e85744b7e1c0f722a4b8a2f8b8a02327f46e85586f4e7c5cbd6a`。
- 外部 `verification-evidence-manifest.json`：70 个文件、3483201 bytes，SHA-256
  `02555e72b96c977a1597da91f24db7d7301e8822222c31d3148f3481f1c7b25a`。

证据根：`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/RD3-D0/20261012`。
该外部清单冻结 V 报告前的原 ZIP/解压、用户 preflight、检查过程、失败日志、结果与工具文件；
排除精确 I 标识的 clone 源/其 Git metadata、清单自身和随后交付/push 记录。未声称覆盖其他历史缺失目录。

主要复现材料：`qualification/commands.json`、`retry-commands.json`、`09-document-gate.log`、
`results/checks.json`、`results/result.json`、`implementation-file-list.json`、`implementation-staged.patch`。

## 变更清单和授权边界

I 的精确 48 文件清单为 [D0_MANIFEST.sha256](D0_MANIFEST.sha256) 的 47 项加该 Manifest 自身。
其中 38 项为原包；新增审查材料为：

- `D0_REVIEW.md`、`D0_ARCHITECTURE_REVIEW.md`、`D0_PROVENANCE.md`、`D0_P1_ENTRY.md`。
- `D0_TRACE_REVIEW.csv`、`D0_MIGRATION_MAP.csv`、`D0_TYPE_REVIEW.csv`、`D0_SOURCE_ANCHORS.json`。
- `checks/verify_d0.py`、`D0_MANIFEST.sha256`。

V 只新增 `D0_VERIFICATION.md`，总新增 49 个文件。
ALLOWED 仅上述 `docs/render-v2/rd3/**`；旧 RD2/FINAL、历史报告、Legacy、用户工作区及生产/测试为 READ-ONLY。
FORBIDDEN 的生产 C++/CMake/现有测试修改、main 合并、P1 实施均未发生。
正式旧 RD2 权威替换标记未写；本轮用户审查后方可作出该批准。

## 结论与停止点

**RD3-D0 = PARTIAL**；原包导入和机械保护 PASS，语义完整冻结仍有上述必需未决项。
建议先审查并裁决 R01–R04、采纳 R05 的安全条件，再明确批准 RD3 设计和 P1 范围。
P1 最小生产范围仅 Client/Server 装配拆分、中立 Scene owns View/Target、强身份协议边界与受影响消费者/构建闭包；
不提前实现 P2–P5，不重写已正确 C0–C13。详见 [P1 入场建议](D0_P1_ENTRY.md)。

按用户授权仅将本轮 I/V 正常快进推送到 `codex/render-v2`，推送完成后 STOP。
不合并 main，不自动进入 P1。
