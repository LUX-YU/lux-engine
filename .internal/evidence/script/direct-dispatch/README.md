# 脚本直接派发优化：证据入口

实际代码、正确性和生成安装已交付；**性能未收口**。新增首次 prepare 和首次 Hook/Event 登记回退
已确认，不用其他路径收益抵消，不认定其为必要安全成本。停止独立审阅，不合并 main。

- B0：`402d5ca5ba0c2728bb4bdedda289f9eb36ce7e4e`（入口 `0ceedbcd` 生产内容相同）。
- 资格源码：`bc2dbfe2270a4b6777207deceb1ad33b78a02920`，生产与 `b5de3b6a` 相同。
- 后续 `ee394cd7` 仅修诊断容量；`611f2750` / `aa855b20` 实验均已撤销。文档提交不是新测试身份。
- lux-cxx：`3100f54d0743c5ed94a4ccf5943df04e933de255`；toolset：`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`。
- 完整 ZIP：71,284,473 bytes，5,740 个条目，SHA-256 `fde29f91b2fa9a224bed08d353698ae09886304c83bf58398cfdcae0e8b13004`。

- [direct-dispatch-raw.zip](direct-dispatch-raw.zip)。

归档为普通 ZIP。
[逐项原路径/大小/哈希](raw-files.json)、[SHA256SUMS](SHA256SUMS)、
[源/安装身份](identity.json)、[安装依赖闭包](installed-link-closure.json)、[归档验证摘要](archive-summary.json)。
所有归档条目已逐项读回核对。无编译 EXE/DLL/PDB/LIB/OBJ；实际产物留在本地受控目录并保存哈希。
22 个 VTune 原始采样项目的 SQLite/采样数据属于诊断记录，随包保存。

| 包内入口 | 实际内容 |
|---|---|
| `evidence-stage/delivery.json`、`logs/delivery-{t,d,l}` | clean tracked clone、all-j4-k0、no-op、install；109/123/110 通过 |
| `logs/restored-t`、`evidence-stage/final-audit.json` | 撤销实验后同 bc2 源码重新链接的 Toolchain；独立哈希、all/no-op/109 通过，不重标此前计时产物 |
| `delivery-consumers`、`delivery-incremental` | 15 个 installed consumers、13 类增量；Native 特化/动态 publication、真实 Lua provider 全链 |
| `delivery-relocation` | 旧 SDK/source/build 离线，迁址重新生成编译执行；legal=2/rejected=2，生命周期/provider/lease 各释放一次 |
| `delivery-traces` | B0→候选原非空生命周期/Event 轨迹、旧断言和 wire 288 字节 golden；prepare/late/remount 五对 |
| `delivery-protocol` | 原 frontier、17 READY、预算 3、真实 step 1～6、关闭前排空 |
| `delivery-scale` | 8/8192、16 warmup+128 单实例重建；稳定地址、资源峰值和提交槽位/endpoint 计数 |
| `hook-ratios-2`、`logs/qualified-observation` | 0/50/99/100% 挂起；独立计时与真实访问计数，不把关闭观察时的零当实测 |
| `performance/delivery`、`performance/extra`、`performance/delivery-values` | 130 个主进程、C++ coroutine/Physics 20 个额外进程、typed/record 五对及诊断 |
| `performance/delivery-ipo`、`locality-2` | 直接/静态/动态/Native 特化、真实三语言交错/独立分组诊断；生产顺序不变 |
| `performance/register-confirm`、`cold-confirm-b5`、`cold-phases` | 新登记/首次准备回退的确认和分段定位；未获豁免 |
| `performance/*`、`scale-*` | 各独立提交对照、确认组与变慢样本；无收益实验保留原结果并撤销代码 |
| `memory-diagnostics`、`evidence-stage/delivery-summary.json` | VM/Native/EXE-local 内存范围、完整分布/尾部/业务量/backlog；旧缺失字段仍未知 |
| `qualified-vtune/analysis.json`、`vtune-projects` | 22 次软件样本、inline/physical/调用栈、生成模块与实际加载映像身份 |
| `qualified-vtune/ambiguous-search-reports`、`exact-finalization.json` | 最初同名 DLL 搜索误标的无效符号报表与六份修正后的 FlowForge 解析；原始样本未修改 |
| `evidence-stage/diagnostics` | 69 段 runtime + IPO 机器码、PDB 地址/哈希、实际诊断源码；工具最近 export 标题不代替真实符号 |
| `evidence-stage/trial-decisions.json` | 编译/探针失败、真实语义负例、撤销与成本未收口原因；不丢慢样本 |
| `evidence-stage/protection-check.json` | main 未由本任务修改、六项旧哈希保持、其他外部变化只记录；依赖未升级 |

重放脚本位于 `evidence-stage`，使用对应 clean source 和固定依赖重新配置受控构建槽位；
不要将旧 source/build/generated 补作新资格输入。已有 SR-2～SR-6/区域优化证据不重打包。
硬件采样探针失败，未验证 PMU cache/branch 指标。无 Android/其他 CPU/真实游戏资格。
软件样本包括启动/warmup/输出，不能用它们的总时间替代无采样配对。部分符号未知仍明确保留。

[主报告：实现、所有权、旧断言对应、完整成本、实验决定与限制](../../../script-system-direct-dispatch-optimization-2026-09-08.zh-CN.md)。
