# RD3-D0 来源、历史资格与验证协议

## 实际输入和 Git 身份

| 项目 | 已核查身份 |
|---|---|
| Repository | `https://github.com/LUX-YU/lux-engine` |
| Remote branch / actual baseline | `codex/render-v2` / `246fe21eb6d060801b9a16ddcadde2be89dbbecc` |
| F6.b Implementation I | `7406965112f09807b9f70aa3e739ff43539969c8`，真实 parent `fb229ba44fa8211c19e1eefd2717f0f7c3f0e972` |
| F6.b Verification V | `246fe21eb6d060801b9a16ddcadde2be89dbbecc`，真实 parent 为上列 I |
| Legacy frozen source | `a669409a289a6fa4092f21176397795b1cdb7f3e`，按仓库 Legacy manifest 比较 719 个源 Blob/Mode/Size |
| 输入 ZIP | `RenderV2_RD3_EffectGraph_Architecture_and_Implementation_2026-10-12.zip`，170039 bytes |
| ZIP SHA-256 | `3ac411f5c6d000b1a7b7a3ffd0e8e3e0b0aeb45936a91433065f9cbc7ead88af` |
| 独立附件合订版 | `FINAL_RD3_85E7A090E8A289%88.md`，119331 bytes，与 ZIP 内合订版字节相同 |
| 合订版 SHA-256 | `401aa86b5ba881548c993f0ea8c23311e91227b24d4402cf26a756fb66abf459` |
| 输入 Manifest SHA-256 | `ec5ccf605347e07d4a38ce94d049d3a18466b92cc5e3d1316f9fc4707aed3468` |

附件位于 `E:/Resources/3DModels/`。完整安全解压到源码外 evidence/input 后阅读所有章节、LAYOUT、
工作单和四表；全部 38 个文件再按原字节导入本目录。原 Manifest 覆盖其余 37 个文件，自身通过上列哈希锁定。
合订版包含全部 21 章原文；它与单章是重复呈现，不是额外架构授权来源。

当前 checkout 原先为浅克隆，仅为核对补取 F6.b parent 和 Legacy 指定提交，没有 reset/clean 用户工作区。
I 使用 managed worktree `C:/Users/chenh/.codex/worktrees/render-rd3-d0/lux-engine` 的 `codex/rd3-d0` 分支施工。
原仓库 `E:/SyncForder/CodeRepos/lux-engine` HEAD/dirty diff 保留。远端推送使用非 force fast-forward。

## 历史状态（只读报告引用，不是 D0 重验）

| 既有资格 | 原报告的结论 | 本轮处理 |
|---|---|---|
| F4 / F4-C-P | PARTIAL；C9 p95 +3.4483%，CI95 [-1.4085%, +12.0690%]；完整 candidate +2.9952%，CI95 [+0.3965%, +6.8838%] | 原有限延期与不确定性保留；不宣称关闭 |
| 原 F5 | 功能/生命周期通过；Record/Submit p95 +1.4925%，CI95 [-3.6199%, +7.9439%]，整体 PARTIAL | 第 16 章引用的是此历史时点 |
| 后续 F5-C | Normal/ASan 175/175；Record/Submit p95 +0.3322%，CI95 [-8.2126%, +5.5749%]，整体 PARTIAL | 同时保留此更新时点，不覆盖旧 F5 样本 |
| F5-C-D | 48 主进程 + 8 局部观测；历史大幅 p50 未重现，四 View 尾部仍 INCONCLUSIVE | 不是性能等价证明，没有新 5% PASS |
| F6.b 功能 | 原报告 Normal/Full ASan 200/200，GPU/ABI/lifetime PASS，Validation errors 0 | 引用原资格，本轮未重跑 |
| F6.b fixed binding | p50 -1.6949%，p95 +0.4000%，原限定 gate PASS | 不扩大为全部 native 性能通过 |
| F6.b Record/Submit | p95 +5.0936%，CI95 [-13.7143%, +17.8182%]，PARTIAL | 不能证明或排除因果生产回退 |
| F6.b cold layout | p50 600→1700 ns；10→24 allocations；REGRESSION_OBSERVED | 非历史延期豁免，保留 full structural identity 正确性 |
| 产品/Legacy/H | V2_PRODUCT EXPECTED_UNAVAILABLE；F/H 不因机制测试完成 | 本轮仍无业务/产品/高级算法 PASS |

证据源：上级 `F4CP_VERIFICATION.md`、`F5_VERIFICATION.md`、`F5C_VERIFICATION.md`、
`F5CD_VERIFICATION.md`、`F6B_VERIFICATION.md`。其 Git Blob、SHA-256 和大小记录在 D0_SOURCE_ANCHORS.json。
F6.b 报告本身披露的旧中间 PDB 改变、一次开发失败日志丢失等例外仍原样保留；没有静默消除。

本机未找到这些原始归档入口：

- `D:/LuxQualification/render-v2-f6b/evidence-manifest.json`
- `D:/LuxQualification/render-v2-f5cd/evidence-manifest.json`
- `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F4-C-P/92b126f67873ccbb576d0ffc5f1330964d6f84f6/evidence-manifest.json`

因此本轮能证明既有 Git 源码/报告完整保留，**不能独立重算历史性能原始样本或声称原始归档全文件 SHA 重验 PASS**。
未找到不等于本轮删除。没有联网检索或替换历史样本，也没有新实验改变旧判定。

## 独立验证方法与可复现命令

I 只新增本目录。提交前检查 diff 目录/原文件 Blob、输入 Manifest/UTF-8/CSV；提交后运行
`cmake -DLUX_SOURCE_DIR=<I-worktree> -P cmake/ValidateTrackedSnapshot.cmake`。
随后 `git clone --no-hardlinks --no-checkout <I-worktree> <evidence>/qualification/source`，
固定 clone 的 `core.autocrlf=false`，detached checkout 精确 I，再运行 tracked snapshot 和下列脚本。
Git objects 不得使用 alternates/hardlinks；源必须 clean，验证中不修复内容。

```text
py -3.13 <clean-source>/docs/render-v2/rd3/checks/verify_d0.py
  --source <clean-source>
  --implementation <exact-I-SHA>
  --require-clean
  --zip <original-ZIP>
  --combined <original-separate-combined>
  --user-repo E:/SyncForder/CodeRepos/lux-engine
  --user-before <evidence>/user-worktree-before.json
  --user-patch <evidence>/user-worktree-before.patch
  --output <evidence>/qualification/results
```

上面分行仅为可读性，实际命令必须按所用 shell 的参数语法调用。检查材料不编译生产代码、不会修复源码。
机械 checks 与设计冻结结论独立输出：脚本 exit 0 只代表机械检查通过，不能把 R01–R04 自动关闭。
检查输入完整性、全部表主键/外键/原 oracle、源码和迁移锚点、类型补充、相对文档链接、Manifest、
基线全部 tracked 文件、Legacy 719、用户 HEAD/status/binary diff/七个配置文件字节以及 clone 独立性。
外部网页链接没有请求访问，不声称检验引用网站的内容或在线可用性。

## 证据和双提交

源码外根：`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/RD3-D0/20261012`。
包含原 ZIP、完整解压、用户 preflight、原 Git tree、实施前检查、独立 clone 和 results/raw logs。
实施生成辅助工具只在该目录；仓库中的 checks 是 I 冻结的只读检查器。
V 只新增 `D0_VERIFICATION.md`，记录精确 I、I parent、Manifest SHA、检查结果与所有未决项。
不将验证环境隔离夸称为第二个人或独立代理审查；本轮由同一执行代理在独立 Git clone/进程完成复核。
V 的确切 SHA 在提交后记录到外部 delivery.json，避免报告自引用。

本轮没有生产构建、测试删除、性能实验、Android/Linux 资格或 main 合并。I/V 推送后 STOP。
