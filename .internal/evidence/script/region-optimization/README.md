# ScriptSystem 区域优化：原始证据

本包记录本轮受控区域、Event 登记、FlowForge O2/frame 和有限 Lua 实验。
原 SR-2～SR-6 与此前 VTune 包不改写；本轮归档提交不作为新资格源码。

- 入口参照：`5a309191f08f43e220a9309961f9de677de9d071`；其生产代码与 `7b5e1dd4/f64cadde` 相同。
- 最终资格源码：`402d5ca5ba0c2728bb4bdedda289f9eb36ce7e4e`，独立 clean clone，RelWithDebInfo。
- 后续 `c3bf545c` 只修正 trace 驱动的 3 个跨批次 Hook 区域入口；未更改已资格的生产代码。
- lux-cxx：`3100f54d0743c5ed94a4ccf5943df04e933de255`；toolset：`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`。
- 归档：[region-optimization-raw-evidence.zip](region-optimization-raw-evidence.zip)，46,184,160 bytes，3,273 项。
- SHA-256：`e298cbb859fc77503f8d3b559be87c7336d128370b6dbe0bf8bc350cc283a035`。
- [逐项索引/大小/哈希](raw-files.json)、[SHA256SUMS](SHA256SUMS)、[源码/安装身份](identity.json)、
  [实际消费者链接闭包](installed-link-closure.json)、[归档验证摘要](archive-summary.json)。

所有条目已逐项读取验证。包不含 EXE/DLL/LIB/OBJ/PDB；包含 288 字节 wire golden 和 18 个 VTune
原始采样项目（配置、样本、SQLite 数据库）。采样数据库属于诊断数据，不是编译产物。
实际 AOT DLL、匹配 PDB 与基准 EXE 保留于本地固定路径，仓库保存其 SHA 和反汇编；
在其他机器恢复源码/符号时须使用相同源码与工具链，不把找不到模块自动解释成零开销。

| 包内目录 | 可核对内容 |
|---|---|
| `evidence-stage/identity.json`、`final-protection-check.json` | 入口、主工作区七项未知修改、依赖；完成前逐文件哈希核对 |
| `evidence-stage/qualification.json`、`logs/final-{t,d,l}/` | clean tracked gate、configure、all-j4-k0、CTest、no-op、install；109/123/110 全通过 |
| `final-consumers/`、`final-incremental/` | 15 个安装消费者与 13 项真实生成增量；SDK Ability/provider/结果/关闭闭环 |
| `final-relocation/` | 新 SDK/source 位置；旧输入离线后的重新生成/构建/运行及路径审计；首次误报另留 |
| `final-traces-2/` | 六组生命周期、八组 Event、wire golden、五对 prepare/late/remount 与 EXE-local 分配 |
| `final-scale/` | 8/8192 配置；16 warmup/128 单实例重建，固定地址与 slot/endpoint 访问计数、尾部 |
| `final-protocol/` | 17 READY、预算 3、实际 step 1～6、关闭前全部完成且 backlog 0 |
| `final-storage/` | 相同 Native 观察器、O2/最终机器 frame 声明、backing/live/metadata、业务和最终释放 |
| `performance/final/` | 130 个进程、每一行真实工作匹配、总时间、摊销成本、分位数、错误可观测性 |
| `performance/final-values/`、`lua-confirmation/`、`values-confirmation/` | typed/record 主配对及 Lua 有限复核；确认组不替换原组 |
| `performance/entry-event/`、`flow-o2/`、`flow-frame/` | 阶段贡献，开发 patch 与真实身份分开，不将中间候选当最终参照 |
| `performance/lua-owned/`、`lua-cache-off/`、`lua-owned-values-2/` | 已撤销 authority cache 与默认 owning record；真实失败、完整链路回退与孤立输出收益 |
| `typed-region-probe/`、`lua-destroy-probe/` | 两次有限诊断；无生产改动结论、全部有效慢样本及缺功能诊断限制 |
| `final-vtune/resolved/`、`final-vtune/diagnostics/` | 匹配 PDB/AOT 后的热点/调用栈、实际汇编、inline 地址检查、产物身份 |
| `typed-region-probe/profiles/resolved/`、`vtune-projects/` | typed 入口/最终四份采样；全部 18 个原始项目 |
| `evidence-stage/diagnostics/`、`snapshots.json` | 实际生成/诊断源码快照；平铺短路径以避免 Windows 长路径，原位置和哈希见映射 |
| `evidence-stage/trial-decisions.json` | 失败、修复后重跑、有效但撤销实验、异常样本的原因与去向 |
| `evidence-stage/cleanup-20260908/`、`retired-development/` | 清理完成清单与结果；149 个旧构建树释放约 103 GiB；源/依赖/未知修改受保护 |

全部重放脚本在 `evidence-stage/`；使用实际资格源码的仓库驱动（trace 使用后续明确修正的驱动）。
目录参数必须按新环境填写，不从旧 build/generated 补齐源输入。构建、测试和正式计时串行。
清理的 139.86 MiB 元数据 ZIP 留在本地，包内保存各 ZIP 哈希及删除清单，不重复塞入历史构建产物。

[主报告：实现、所有权、旧断言迁移、完整成本与限制](../../../script-system-region-optimization-2026-09-08.zh-CN.md)。
正确性和安装通过不表示性能等价：FlowForge/Event 登记改善；Lua Update、typed 和 coroutine p99
新增成本未全面收口。未授予所有残余为必要安全成本，也不取消原 SR-6 历史债务。
原始 [VTune 入口证据](../vtune-2026-09-08/README.md) 保持原身份。
