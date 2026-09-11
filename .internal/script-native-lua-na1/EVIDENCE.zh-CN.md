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
