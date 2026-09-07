# SR-5 首批值转换原始证据

[收口报告](../../../../script-system-sr5-closeout-2026-09-07.zh-CN.md) ·
[安全/构造/支持矩阵](../../../../script-system-sr5-implementation-2026-09-07.zh-CN.md)

- [SR5-raw-evidence.zip](SR5-raw-evidence.zip)：16,341,809 字节，1,763 个条目；不含编译二进制。
- [SHA256SUMS](SHA256SUMS)：`8cb121fdeebb157aaadcff09c688a32850df17285759fdc8b0cb0f7ea3313438`。
- [raw-files.json](raw-files.json)：逐条路径、大小与 SHA-256。
- [identity.json](identity.json)：源提交、clean 状态、安装头/工具/DLL 实际哈希；私有 owner 头未泄露。
- [installed-link-closure.json](installed-link-closure.json)：direct runtime 与 description leaf 的实际安装链接检查。

最终生产身份 `6086e4a4dfd337ddad522a7a71c7b7d14b571189`；先前 4fe3565e 的初轮数据独立保留，不改标签。
最终 q 目录中的日志是 6086e4a4；q/qualification.json 包含追加的各轮身份。b7/b8-qualification 为前两轮快照。

| 归档目录 | 内容 |
|---|---|
| q | 三种 RelWithDebInfo profile 的 clean tracked 检查、configure/all/CTest/install/no-op、实际生成与编译命令 |
| consumers-final / incremental-final | 最终 15 安装消费者与 13 项增量预期验证；生成输出另见 evidence-stage/diagnostics |
| performance-final | 最终八场景、五组独立进程配对、完整逐行工作量验证与摊销成本 |
| performance | 4fe3565e 初轮同量配对；不能当作最终版本数据 |
| value-costs-valid | 旧手写/新生成输出的同量探针，新双向 typed Ability 绝对成本；分配诊断与计时分开 |
| probes / scale-replay | 本轮 4fe3565e 的 wire/非空轨迹/冷期/8与8192装配规模；保留真实源码及精确命中数 |
| diagnostics | 两轮反汇编、正索引调用位置、二进制变化、成本原始汇总；静态位置数不是动态调用计数 |
| evidence-stage | 开工与最终保护状态、原始失败/无效试验、重放脚本、最终 EXE/DLL 哈希和实际生成头 |
| boundary / b7-qualification / b8-qualification / consumers / incremental | 边界先行与中间验证，明确保留其原身份 |
| value-costs | 缺少 compile_commands.json 的无效首轮；未执行失败候选，不纳入时间配对 |

计数关闭时日志中的 0 不是“零分配”；旧 benchmark 没有的错误字段继续记 null。
Lua Ability 残余和生成 record 推送新增成本尚待审阅接受，不宣称性能等价或全部成本已收口。

固定提交下载与远端重新取得后的校验将在推送后补入本索引。
