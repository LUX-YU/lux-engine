# 当前 ScriptSystem 的 VTune 再采样

本轮分析入口 `3ccef2ba4184eb81f6d4830c2137a5f3ab3f9957`，生产源码和所用产物仍绑定
`402d5ca5ba0c2728bb4bdedda289f9eb36ce7e4e`。新采样不重标为新构建/正确性资格。
无生产改动、无新构建树；仅 RelWithDebInfo / Windows x64 / LuaJIT 的脚本基准 CPU 画像。

- [完整结论与源码调用链](../../../script-system-vtune-refresh-2026-09-08.zh-CN.md)
- [原始归档](vtune-current-raw.zip)：24,639,728 bytes、1,203 项，含 18 份有效新采样及无效尝试。
- SHA-256：`9e7406556868a9bbe7b3eeba456c32ad4288f7e62e0d7b9b4618082c7bd01334`。
- [逐项文件/哈希索引](raw-files.json)、[SHA256SUMS](SHA256SUMS)、[归档核验摘要](archive-summary.json)。

所有项本地逐项读取并核对大小/哈希。不含编译 DLL/EXE/PDB；含 VTune 原始采样项目、SQLite 数据库、
未处理 CSV、完整调用栈、匹配符号后的物理/内联热点、8 份新汇编导出、应用 stdout、完整工作量和输入身份。
实际 DLL/PDB 在原本的本地资格/采样位置保留，其哈希见 identity/generated-images。

| 包内入口 | 内容 |
|---|---|
| `identity.json`、`final-identity-check.json` | 生产/源码/产物、VM/PDB、依赖、实际 Lua 资产；main 未知修改保护 |
| `runs.json`、`*.application.log`、`workload.json` | 18 份有效执行与逐行业务检查；无效长采集另保留 |
| `analysis.json` | 全进程 inclusive / inline self / physical self；范围相互重叠时不相加 |
| `*-summary.csv`、`*-physical.csv`、`*-top-down.csv`、`*-callstacks.csv` | 原始 VTune 导出 |
| `*-sw-*/` | 原始项目 config/data/sqlite；复查需对应符号、源码和工具环境 |
| `assembly/` | 新汇编、inline 地址包含性、同步返回的比较/状态判断顺序 |
| `generated-images.json`、`code-snapshots/` | 当场捕获 AOT DLL 哈希、实际生成 typed/record 头快照 |
| `hardware-capability.json`、`hardware-probe.log` | 硬件采样不可用；没有 cache/IPC/分支误预测结论 |
| `initial-validation-runs.json`、`validation-correction.json` | 首次输出 schema 校验误报，原应用样本保留并复核 |
| `flow-update-10k-sw-0*`、`linker-instrumentation-failure.json` | 遥测子进程拖住采集、VTune 注入 linker 断言；均未混入最终占比 |
| `ProfileRefresh.py`、`AnalyzeRefresh.py`、`ValidateRefresh.py`、`AssemblyRefresh.py` | 采样/解析/工作量/汇编脚本；CSV 解析保留树形缩进并处理带逗号的模板名 |

脚本中绝对路径用于绑定这次输入，跨机器重放应改参数并重新验证身份；不能假定任意旧 build 与此等价。
旧性能债务、上一轮有/无优化的配对结果仍见原报告。采样百分比变化本身不证明性能变化。
Lua Event 的实际恢复预算为 size=10,000，FlowForge Event 为 2,000；两者不是同量语言横比。
