# SR-3 热路径成本修正证据（2026-09-07）

**状态：已实施并验证，成本仍未收口。** 固定 B0=`8145598c18421d03da1dac21251200af3290e27d`，
B1=`3e2bcbd29580d2e744744b64149b7d8fedcc8950`，最终生产 B2=`99c1d095fad6a728c3d808ff2d853ce84b59bfea`。
中间候选 C2=`9406f72c380cec6e119e50499f392db38d92eaca`；C2 记录单列，不替代 B2。

完整解释与结果：[专项报告](../../../script-system-sr3-cost-closeout-2026-09-07.zh-CN.md)。
本轮主文档增量：[SR-1 主文档 §15](../../../script-system-sr1-design-2026-09-06.zh-CN.md#15-sr-3-热路径成本修正与未收口结论2026-09-07)。
旧 SR-2/SR-3 四包和旧提交身份不变，没有重新上传或改标签。

## 1. 固定提交与下载

两包固定归档提交：`7129a486d01c741f714f2831693e8d777c351794`。文档、索引说明和远端校验记录后续提交不改变包。

| 包 | 仓库相对入口 / 固定下载 | 条目数 | ZIP bytes | SHA-256 |
|---|---|---:|---:|---|
| 最终 B2 | [final/SR3-raw-evidence.zip](final/SR3-raw-evidence.zip) · [固定提交下载](https://github.com/LUX-YU/lux-engine/raw/7129a486d01c741f714f2831693e8d777c351794/.internal/evidence/script/sr3-cost/final/SR3-raw-evidence.zip) | 1,396 | 10,690,313 | `cb3d49cd184f5d4137fe519877a69ae92e0d8c56971c44fd3305c22e72e2d098` |
| 复核/归因/C2 | [investigation/SR3-raw-evidence.zip](investigation/SR3-raw-evidence.zip) · [固定提交下载](https://github.com/LUX-YU/lux-engine/raw/7129a486d01c741f714f2831693e8d777c351794/.internal/evidence/script/sr3-cost/investigation/SR3-raw-evidence.zip) | 2,269 | 27,991,260 | `e6a489a5e73fe5f5de7c4d2a108ce5f9e83f17ccf9e395c0229920ab9228345b` |

逐文件路径、大小和哈希：[final/raw-files.json](final/raw-files.json)、
[investigation/raw-files.json](investigation/raw-files.json)。安装/源码/生成器身份分别见
[final/identity.json](final/identity.json)、[investigation/identity.json](investigation/identity.json)。
实际链接闭包见两个目录的 `installed-link-closure.json`；整包校验见各自 `SHA256SUMS`。
[远端重取校验记录](remote-verification.json) 记录 GitHub fetch 的固定 SHA、二进制读取方式、两包哈希和全部文件匹配数。
远端校验不依赖浏览器匿名访问私人仓库，也未修改已有 transport 的未 checkout 工作树。

## 2. 包内可重放入口

| 主题 | ZIP 内相对路径 |
|---|---|
| B2 完整执行命令 | final: `sr3-cost-final/run.ps1`、`run.log` |
| B2 clean clone / 两 profile 全量构建、CTest、no-op、安装 | final: `sr3-c3/qualification.json`、`{t,d}/{tracked,configure,all,ctest,second-build,install}.log`；实际 no-op 日志名以 raw-files 索引为准 |
| 七点重入、同步错误、Bindings、新票据及其他真实断言 stdout | final: `sr3-c3/{t,d}/Testing/Temporary/LastTest.log`；`sr3-cost-final/diagnostics/verification.json` 精确检查七点+同步分支、/UNDEBUG、六项历史失败及旧证据 blob |
| 14 安装消费者 | final: `sr3-c3-checks/consumers/consumers.json` 及各消费者 configure/build/run 原始日志 |
| 轨迹、严格命中数与 wire | final: `sr3-c3-checks/probes/manifest.json`、`{baseline,candidate}/{trace,event-trace,wire}.log`、`instrumentation.json`、`v1.bin`；B1/B2 补充在 `sr3-cost-final/b1-b2-probes/` |
| 三方同量性能全部进程/CSV/业务比较 | final: `sr3-cost-final/final-{b0-b1,b1-b2,b0-b2}/{runs,business-comparisons}.json` 及逐 pair CSV/log；总时间和归一化在 `sr3-cost-final/cost-results.json` |
| 冷期/规模完整分布 | final: `sr3-cost-final/diagnostics/cold-scale-results.json`；B0/B2 原始在 `sr3-c3-checks/{probes,scale}/`，B1/B2 在 `sr3-cost-final/b1-b2-{probes,scale}/`；B0/B1 在 investigation: `sr3-closeout/b0-b1-{probes,scale}/` |
| 独立分配诊断 | final: `sr3-c3-checks/flow-allocations/manifest.json`、各目录的 `*-allocations.csv.log`、规模 diagnostics CSV；稳态另有 diagnostic 进程 |
| B0/B1 本轮复核和六项历史失败 | investigation: `sr3-closeout/reference-refresh/{sr3-reference,sr3-final}/`，最初同量测量 `sr3-closeout/b0-b1/` |
| 实际采样与机器码/编译报告 | investigation: `sr3-closeout/diagnostics/` 的 B0/B1/C2 RIP/OBJ/DLL/编译器记录；final: `sr3-cost-final/diagnostics/` 的 B2 `b2-{cpp,flow,event}-profile/`、OBJ/DLL、layout 和 26 exports 比较 |
| 保留/撤销实验 | investigation: `sr3-closeout/diagnostics/` 中的 `split/layout/ticket/boundary/combined/visitor/compact/compact-only` 与 `lookup/event-access/suspension` 的 patch、构建日志、runs.json；`invalid-trials.json` 说明无效原因 |
| 中间 C2 资格与性能 | investigation: `sr3-c2/`、`sr3-c2-checks/`、`sr3-closeout/final-*`；这些文件名中的 b2 是当时的 C2，实际 SHA 为 9406，未改名冒充 99c1 |
| 失败的长路径资格 | investigation: `sr3-closeout-candidate/`；MSVC C1083 后使用新的短目录，未运行旧 EXE |
| 依赖与机器身份 | investigation: `sr3-closeout/start-identity.json`、`cxx-header-check.json`；两包 `identity/identity.json` |

归档只包含原始日志、CSV、文本反汇编/编译器输出、命令/脚本、测试仪器化源码、身份清单及 288-byte wire fixture。
不包含 DLL、EXE、OBJ、PDB 或 LIB。诊断编译器输出的对象地址/内联位置保留；实际构建二进制以路径和 SHA 识别。

## 3. 验证与限制

Toolchain 107/107；Developer 113/119，失败仍为六个历史 Scene Lua 断言。14/14 消费者、6+8 严格轨迹、wire、
8/8,192 规模业务与资源计数通过；两 profile 的第二轮全量构建无工作，安装成功。限定 RelWithDebInfo，无 Android。
当前生产代码和最终资格绑定 99c1；后续仅增加报告和证据，不用后续文档提交的 SHA 改写 EXE 身份。

三方正式每场景五对独立进程。B0→B2 C++/FlowForge update 中位 -7.06%/-8.14%，但 FlowForge Event
仍 +13.17%（五对全部正回退），约 +57.14 ns/实际 resume、+0.1143 ms/本例帧。Lua Event、迟到及小规模
重建也有残余；全部批次时间、有效操作数、backlog、分布、内存增加和异常值在报告及 JSON 中保留。
误差、异常值和不能精确归因的成本不被删去，也不自动认定为必要安全成本。

采样是用户态主线程 RIP 墙钟抽样，含启动/关闭和暂停扰动，没有 ETW 周期/硬件 cache miss。正式性能另行无采样。
早期迭代 EXE 的内嵌 stamp 可旧，由 DLL SHA+patch 识别，不作为最终资格。稳态旧 CSV 缺 failures/error 总数，
汇总为 null；成功退出、准确业务完成、backlog 另验，不能写成系统错误总数为零。EXE-local new 不等于全 DLL 堆统计。

停止于 SR-3，交付独立审阅；不进入 SR-4/5/6，也不以职责拆分的架构收益抵消剩余成本。
