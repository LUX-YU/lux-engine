# V4-H1 原始证据入口

最终实际资格源码：`7c565a0014d7c82f0d522eb4ca8c8f0f64773893`。
固定 V4 对照：`f7d2815bdd2025ee23a7c11449def822413f58e9`。
H2 撤回前候选：`655855401e09aac2d4e8becd16afca62086291ee`。
本索引和报告提交不代表新测试或计时身份。

- [结果与限制](../../../script-v4-h1/RESULT.zh-CN.md)
- [逐谓词检查审计](../../../script-v4-h1/CHECK_AUDIT.csv)
- [真实断言矩阵](../../../script-v4-h1/TEST_MATRIX.csv)
- [本轮 V4 采样说明](../../../script-v4-h1/PROFILE_BASELINE.zh-CN.md)
- [结构化结果](../../../script-v4-h1/result.json)
- [原始归档](v4-h1-evidence.zip)、[SHA-256](SHA256.txt)、[包身份](MANIFEST.json)
- [固定归档提交下载](https://github.com/LUX-YU/lux-engine/raw/2372e0a38b8bc8f917e09cd7660640163b5dfc42/.internal/evidence/script/script-v4-h1/v4-h1-evidence.zip)

归档提交 `2372e0a38b8bc8f917e09cd7660640163b5dfc42`；13,210,304 bytes，
1,589 条目，其中 1,588 个文件均逐项复核 SHA-256（另一项是包内 MANIFEST.json）。
包 SHA-256：`f27b20d13c5c921294e4a994c62453532d880e8bfb1209382f5cfea555320a15`。
包含原始 VTune 数据库/导出、源码诊断、机器码、命令、测试/安装及全部计时；不含构建产物或整套 SDK。

## 包内导航与身份

| 包内路径 | 内容及实际身份 |
|---|---|
| `start.json`, `fixed-dependencies.json`, `final-audit.json` | 初始工作区、204 个固定依赖文件、最终镜像/PDB/EXE、main 七个未知文件未改的复核 |
| `baseline-profile/*/result` | V4 五条原始软件采样；同目录 target exit、oracle、summary/hotspots/top-down；完整目标进程范围 |
| `machine-A-full` | V4 PDB 定位的机器码 |
| `machine-B-full` | 撤回前 65585540 的机器码 |
| `machine-B2-full` | 最终 7c565a00 的机器码 |
| `final2-*-build/noop/tests/install.*` | 最终 7c565a00 的 Developer/Toolchain 全量、无工作、CTest 和安装 |
| `final2-*-LastTest.log` | 实际测试输出与业务断言轨迹 |
| `final-vm-tests.log` | 本轮重新执行的固定、未修改 Lua55 VM 四项合同；不是旧 VM 测量 |
| `installed-final2`, `value-incremental-final2`, `relocated-final2` | 最终 15 消费者、13 类生成增量、两条隐藏原路径的迁址链 |
| `final2-costs` | 最终七腿各三对 AB/BA/AB，42 个有效进程，原始 CSV、日志、总时间/分布/业务/错误范围 |
| `final-costs` | 65585540 的七腿 42 个有效进程；**不能作为最终源码数据** |
| `h2-withdrawal`, `h2-withdrawal-costs` | 65585540 仅覆盖 V4 ScriptExecution.hpp 的独立诊断，三个配对、6 个进程；不是 clean 产品源码 |
| `layout-final2` | 最终 A/B 实际 sizeof 和 backing；无新增持久字段；口径不是全引擎 RSS |
| `native-cold`, `native-boundary`, `pin-boundary` | 真 V4/候选 DLL 上的冷拒绝、实际 packet/outcome/ordinal、物理 pin 容量负例；源码与驱动在包根 |
| `contract-comparison-final` | 固定 V4 与 65585540 的八条新增轨迹复跑；最终 CTest 保留相同断言 |
| `contract-comparison3-identity-note.txt` | 早期开发比较的 test/header/runtime 身份字段解释，避免把 761dae91 误标成当时 DLL 来源 |
| `diagnostic-identities.json` | 独立诊断 DLL/PDB/EXE 与测试头的 SHA-256 |
| `attempt*`, `*-driver*.log`, `final2-runner-warning.txt` | 编译/配置/驱动失败和非致命环境初始化警告；未删除失败尝试 |
| `spec` | 用户提供的待实施规格和来源索引；其中模板不是验证结果 |

同包 `final-image` / `final-flow-image` 仅存放 65585540 的身份清单；
`final2-image` / `final2-flow-image` 是最终 7c565a00 身份清单。目录名字不能取代清单中的源码身份。

本机完整原始目录：`E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-v4-h1`。
归档保留全部有效慢组。H2 因成本撤回；其它小幅残余与历史债务仍开放。
没有合并 V4/main、强推或发布 tag。
