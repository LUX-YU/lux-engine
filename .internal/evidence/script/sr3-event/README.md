# SR-3 持续 Event 有限调查证据（2026-09-07）

**结论：未保留新的生产运行时修改，持续 Event 成本未收口。**
B0=`8145598c18421d03da1dac21251200af3290e27d`，B2=`99c1d095fad6a728c3d808ff2d853ce84b59bfea`，
B3 验证快照=`8a6e6ef468f7eb60265678ab25116719b42a01a6`。B3 运行时与 B2 相同，新增测试与计时后观察工具。
lux-cxx 固定 `3100f54d0743c5ed94a4ccf5943df04e933de255`。原 sr2/sr3-gate/sr3/sr3-cost 包与标签不变。

[短主报告](../../../script-system-sr3-event-closeout-2026-09-07.zh-CN.md) ·
[一页 CSV](one-page.csv) · [汇总 JSON](one-page.json) ·
[主文档 §16](../../../script-system-sr1-design-2026-09-06.zh-CN.md#16-sr-3-持续-event-有限调查2026-09-07)。

## 固定归档与获取

归档提交：`c22d719f1adca5de878f465c2fdea1736947faff`。
[仓库 ZIP](final/SR3-raw-evidence.zip) ·
[固定提交下载](https://github.com/LUX-YU/lux-engine/raw/c22d719f1adca5de878f465c2fdea1736947faff/.internal/evidence/script/sr3-event/final/SR3-raw-evidence.zip)。

ZIP **8,897,120 bytes / 1,461 文件**，SHA-256：
`ab214c1ba9a416f3c0984f2b4e4e2a508c8fdd0061ede74abead38deea461537`。
[逐文件大小与哈希](final/raw-files.json) · [源码/安装产物身份](final/identity.json) ·
[实际安装链接闭包](final/installed-link-closure.json) · [远端重取校验](remote-verification.json)。
已使用原有 Git transport 从远端取得固定提交，逐项重新校验全部哈希；没有创建新的上传流程。
不含 EXE/DLL/OBJ/PDB/LIB，保留原始文本机器码、CSV、日志、补丁、诊断源码、构建参数和 288-byte wire fixture。

## 原始记录导航

下表路径相对于 ZIP 根目录；不把探针耗时当作正式性能结果。

| 内容 | ZIP 内路径 |
|---|---|
| 完整串行执行、配置及资格 | `sr3-event/final.ps1`、`final-driver.log`、`sr3-e/qualification.json`；`sr3-e/{t,d}/{tracked,configure,all,ctest,second-build,install}.log` |
| 测试实际 stdout 与精确断言核对 | `sr3-e/{t,d}/Testing/Temporary/LastTest.log`；`sr3-event/diagnostics/{verify.py,verification.json}`，含新增 SINGLE_FLIGHT_OK、8 条 REENTRY_OK、Bindings/authority、/UNDEBUG |
| B0/B2 六项历史失败本轮复跑 | `sr3-event/reference-refresh/{sr3-reference,sr3-c3}/{historical-lua,exit,t-all,d-all}.log`；逐断言与候选比较见 verification.json |
| 14 安装消费者 | `sr3-e-checks/consumers/consumers.json` 及各 consumer 的 configure/build/run 日志 |
| 6+8 严格轨迹与 wire | `sr3-e-checks/probes/`；B2/B3 补充 `sr3-event/b2-b3-probes/`；manifest/instrumentation 与 trace/event-trace/wire 日志 |
| 正式 B2→B3 / B0→B3，同量五对 | `sr3-event/final-{b2-b3,b0-b3}/{runs,business-comparisons}.json` 及各 pair CSV/log；`sr3-event/cost-results.json` 保留所有配对总时间、分母、归一化与独立分配诊断 |
| 匹配 Flow 观察驱动/产物 | `sr3-event/observer-{b0,b2,b3}/identity.json`、构建参数；源码/3 个 generated headers 另在 `sr3-event/diagnostics/observer-inputs/`；`observer-verification.json` 校验 48 次完整性观察 |
| 原工作量真实分支与清理 | `sr3-event/diagnostics/{b2-observed-counts,final-counts}/{results.json,path.log,hits.json,ScriptSystem.cpp,compile.log,link.log}`；探针构建/运行命令在 results.json，时间无效 |
| 当前优化产物 / 26 exports | `sr3-event/diagnostics/{b2,b3}-{system,invoke-resume,exports}.txt`、`machine-identity.json`；导出名称相同不是完整 ABI 证明 |
| 主试验：跳过前免票据 | `sr3-event/diagnostics/gate.patch`、`gate-machine.txt`、`gate-counts/`、`gate-{300,3000}/runs.json`；`sr3-event/gate-{build,build-fixed,tests}.log` |
| 次试验：恢复重叠保护 | `sr3-event/diagnostics/resume.patch`、`resume-machine.txt`、`resume-3000/runs.json`；`sr3-event/resume-{build,tests}.log` |
| 无效/被替代观察 | `sr3-event/diagnostics/invalid-trials.json`：探针源路径错误、初次负例编译错误、非匹配源码的初次分配观察；原日志不删 |
| 冷期、8/8,192 配置 | `sr3-e-checks/{probes,scale}/`、`sr3-event/b2-b3-{probes,scale}/`；`sr3-event/diagnostics/detailed-summary.json` 含全部分布与资源计数 |
| 最终匹配源码 Flow 分配诊断 | `sr3-event/diagnostics/matched-flow-allocations/`、`matched-allocations.py`/`.patch`；两侧 Update/Event 各三帧 0 次 EXE-local new；初次 `sr3-e-checks/flow-allocations/` 仅保留，不作最终匹配对照 |
| 依赖和保护文件 | `sr3-event/identity-check.json`：固定 cxx/toolset、114 安装头、未知修改哈希；`sr3-event/machine.json`：CPU，电源/affinity 与未控制条件见 identity 和 runs |

## 结果与限制

Toolchain 107/107，Developer 113/119；六项 Scene Lua 仍在相同 `activeContinuationCount() == 0U` 失败。
14/14 消费者、严格轨迹/wire、窄回归通过。最终源与构建绑定 clean clone 的 8a6e6ef4，仅 RelWithDebInfo。

Flow Event 300 帧探针：3,000,000 Hook 候选、2,400,000 次跳过、600,000 次 step/resume，backlog=8,000。
机器码确认跳过前有计数写入，提前跳过试验减少票据但新增调用边界；长批中位 +0.97%，撤销。
单独合并 resume 保护中位 -0.51%，未获稳定收益，撤销。没有把这两个观察变成全部残余的根因结论。

正式 3,000 帧/6,000,000 次 resume：B2→B3 中位 +0.30%（-0.50%～+0.90%），
B0→B3 +13.72%（+9.99%～+15.45%），配对差中位 +360.4816 ms，完整帧摊销 +60.080 ns/resume、+0.120161 ms/本例帧。
全部六类场景、冷期与规模分布在 CSV/JSON 中，不以 Update 改善抵消 Event。

Flow observer 源码仅有换行符差异，CRLF→LF 后三方相同，原字节哈希均保留。旧 CSV errors/failures 仍为 null；
新观察是计时后保留错误记录为 0，窄探针另数 recordFailure=0，不新增总错误 ABI。EXE-local new 不代表全部 DLL 堆。
未控制频率/后台负载；没有新的硬件周期/cache-miss 归因。必要安全成本与封装/布局交互的份额仍未隔离。

建议保留 B2 运行时与新增验证；是否接受该性能债务交用户/独立审阅方决定。停止于 SR-3，不自动开始 SR-4/5/6。
