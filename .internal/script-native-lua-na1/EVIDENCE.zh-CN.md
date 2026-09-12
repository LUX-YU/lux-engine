# NA1 证据入口

资格源码固定 `4a8812e9813fe90b241dba29e3e668e4a4239b26`；A 固定 `f7d2815bdd2025ee23a7c11449def822413f58e9`。本页/报告提交不产生新的测试身份。

- [原始归档](evidence/logs.zip)：1366 项、9,950,318 B；含原始 CSV、stdout、命令、VTune DB/导出、构建/测试/安装日志、诊断源码和失败尝试。
- [逐文件清单](evidence/manifest.json)、[归档身份](evidence/archive.json)、[SHA-256](evidence/logs.zip.sha256)。归档 SHA-256：`912b6e2d8f44c94bd6b01e8d73a2b7f2a8b9f061b4abb3baac4b0e2cb4a4eb39`。
- [完整结果](RESULT.zh-CN.md)、[机读结果](result.json)、[54 项映射](CHECK_MAPPING.csv)、[资源账目](RESOURCE_LEDGER.json)、[调用合同](NOTES.zh-CN.md)。

归档内部入口：

| 内容 | 归档内路径 |
|---|---|
| 源码、逻辑提交 | `final-source-identity.json` |
| DLL/PDB/EXE/资产实际身份 | `final-image-identities.json`、`common-library-identity-check.json` |
| 固定依赖与原工作区 | `dependencies-verified.json`、`final-protected-dependencies.json`、`main-external-change.txt` |
| 全部 54 个有效进程与配对 | `final-costs/runs.json`、`final-costs/summary.json`、各 CSV/log |
| 有效 P1/P3 原采样和导出 | `profiles2/`；采样参数、ROI stdout 在对应目录 |
| 有效 P5 原采样和导出 | `profiles-pose/` |
| 四份无效初次观察 | `profiles/`，因未捕获目标业务 stdout 不计有效采样 |
| 全部原 profile 身份 | `final-profile-identities.json` |
| 资源独立观察 | `resources/runs.json`、各 CSV/log |
| 当前机器码与 frame | `machine-code2/`、`current-class-layout2/` |
| 独立 clean clone 资格 | `qualified-d-*`、`qualified-t-*`、`final-vm-tests.*` |
| 16 个消费者 | `installed/consumers.json` 与各消费者 configure/build/run/noop 日志 |
| 旧 13 项增量 | `value-incremental/probes.json` |
| 新 4 项增量 | `new-incremental/results.json` |
| 三条迁址链 | `relocated/results.json`、`relocated/unavailablepaths.json` |
| 补充真实负例 | `supplemental-contract/`、`protocol-probes/` |
| 修正前失败尝试 | `failed-attempt-index.json` 和原阶段日志 |
| 重放脚本 | 根目录 `*.py` / `*.ps1`，保存实际绝对环境路径；在新机器需显式调整前缀 |

编译产物未进 ZIP；它们位于 `E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-native-lua-na1/images/`，完整 hash 在归档。补充合同诊断由独立源码 hash 绑定同一 4a 生产库，不冒充已进入 4a 的新增测试源码。已跑全量 CTest 的 4a 原测试未被更改。

已在本地重新读取 ZIP 并核对全部 1366 项 SHA。远端归档固定提交 `c734758cd2192b5c90ceba796a6663677b753c53` 已通过独立 bare 仓库从 origin 重新 fetch，归档 SHA 和全部 1366 项 hash 一致。

[固定提交下载](https://github.com/LUX-YU/lux-engine/raw/c734758cd2192b5c90ceba796a6663677b753c53/.internal/script-native-lua-na1/evidence/logs.zip) · [远端回取记录](evidence/remote-readback.json)。远端分支之后的文档提交不修改此归档或 4a 资格身份。

## 第二轮交接优化证据

[结果](NEXT-RESULT.zh-CN.md)、[合同](NEXT-CHANGES.zh-CN.md)、[原始归档](evidence/next-hotpath.zip)、[逐文件索引](evidence/next-hotpath.json)。资格源码 7f201916，生产对照 21d10601；不修改以上旧归档或身份。

ZIP 共 741 项、7,244,421 字节，SHA-256 `9fc590df65ea727d31eab552aee9e5b02f8281dc6e323e27288a8dba173bb341`。含两侧 P4 VTune 原始结果与导出、54 个最终计时进程、资源/机器码/布局、全量测试、安装/增量/迁址、镜像哈希、保护检查及所有中间记录，不含编译 EXE/DLL/PDB。已本地解包逐项核验。

| 内容 | ZIP 内入口 |
|---|---|
| 最终同量对照、每批与完整业务 | `next-costs/summary.json`、`next-costs/runs.json`、各 CSV/log |
| 两侧 P4 原始 VTune/导出 | `next-profiles/P4-A/`、`next-profiles/P4-B/` |
| 机器码、布局与 frame | `next-machine-code/`、`next-class-layout/`、`next-analysis/` |
| 资源独立观察 | `next-resources/` |
| 最终构建/测试/安装 | `next-final-*`、`next-vm-tests.*` |
| 16 消费者、值转换和组合增量 | `next-installed/`、`next-value-incremental/`、`next-new-incremental/` |
| 三条迁址链及原路径不可用记录 | `next-relocated/` |
| 基线、最终 EXE/DLL/PDB/资产哈希、固定依赖与受保护工作区 | `next-baseline.json`、`next-final-image-identities.json`、`next-protected-final.json`、`next-source-identity.json` |
| 编译修正及补 sizeof 证明前的完整记录 | `next-first-*`、`next-preproof-*`、`next-intermediate-disposition.json` |

原 C 镜像 126 项、安装依赖 204 项再次核验未变；最终安装公共头与资格 clone 的头逐字节一致。中间四个完整计时及一个被中止进程只归档，不进入最终 54 进程结果。

第二轮归档固定提交 `e23d623684a3f8cf9aca174fcb30a8b6b0020013` 已从 origin 回取；ZIP 与全部 741 项 SHA 一致。[固定提交下载](https://github.com/LUX-YU/lux-engine/raw/e23d623684a3f8cf9aca174fcb30a8b6b0020013/.internal/script-native-lua-na1/evidence/next-hotpath.zip) · [回取记录](evidence/next-remote-readback.json)。此后文档提交不改变 7f201916 资格身份。

## 本地等待与同步步骤优化证据

[实际结果](LOCAL-WAIT-RESULT.zh-CN.md)、[实施合同](LOCAL-WAIT-CHANGES.zh-CN.md)、[机器可读结果](local-result.json)、
[原始归档](evidence/local-waits.zip)、[逐文件 SHA 清单](evidence/local-waits.json)。
资格源码 `841320a36e864145eda43fd6b775a10a696d8e7f`，参照 `7f20191692d3f5e1037a14273a61fc99eef95714`；以上旧归档与提交身份不变。

ZIP 646 项、5093734 字节，SHA-256 `e14fc09449b0371cb1765013afd9274fc5a6a03cf21f40dc5dc8aa53f9dfa5fb`。已本地解包逐项核验，不含编译 EXE/DLL/PDB。

| 内容 | 归档内入口 |
|---|---|
| 完整 54 进程、配对、每批/业务原始数据 | `local-costs/` |
| P4 两侧 VTune 原始采样、命令、调用树 | `local-profiles/` |
| 互斥 CPU 分组、布局/frame、机器码 | `local-analysis/`、`local-class-layout/`、`local-machine-code/` |
| 独立资源和 8/8192 重建复核 | `local-resources/`、`local-scale/` |
| 全量构建、测试、安装、原 VM 合同 | `local-final-*`、`local-vm-tests.*` |
| 16 消费者、13+5 增量、三迁址链 | `local-installed/`、`local-value-incremental/`、`local-new-incremental/`、`local-relocated/` |
| 基线、实际产物、固定依赖、安装头与受保护工作区 | `local-baseline.json`、`local-source-identity.json`、`local-final-image-identities.json`、`local-protected-final.json` |
| 中间失败与有限检查记录 | `local-failed-attempts.json`、各 `local-*.log/json` |

原 D 镜像 126 项、固定安装依赖 204 项核验未变；12 份改变的公开安装头与资格源码匹配。
后续文档提交不重标测试身份。成本仍含 Lua scalar +0.70% 未解释残余；长任务 frame 增大，详情见报告。

