# SR-5 权限补正与有限成本：原始证据

[一页摘要](../../../../script-system-sr5-admission-summary-2026-09-07.zh-CN.md) ·
[完整资格、验证与成本报告](../../../../script-system-sr5-admission-correction-2026-09-07.zh-CN.md)

- [SR5-admission-raw-evidence.zip](SR5-admission-raw-evidence.zip)：13,986,394字节，1,363项；不含编译二进制。
- [SHA256SUMS](SHA256SUMS)：`a30dd8661c917f665400ac7dcb33f039bdb4833e8f87962104a4d90f9edb3cdd`。
- [raw-files.json](raw-files.json)：每项原路径、大小和SHA-256。
- [identity.json](identity.json)：clean源码提交、安装头/生成器/DLL哈希；私有owner头未安装。
- [installed-link-closure.json](installed-link-closure.json)：实际direct runtime及description leaf链接边界。

最终生产资格C1为 `f64caddebcc353c07ac03637135f99586986eb6e`；正确性独立提交 `c29aa8d5b1213ed1898f12caab1a0629860e4628`。
修前仅测试提交为 `31dfc9ca818f15f67cc30ada778dec2f4ebd3c84`，C0/H身份不改标签；原SR-5 final包不改写。

| 归档目录 | 内容 |
|---|---|
| evidence-stage | task文本、开工/结束保护核验、pre/fix独立进程和all日志、成本预注册、脚本与试验patch |
| evidence-stage/diagnostics | 实际生成头、受保护手写诊断源码、优化反汇编及经符号/调用核验的精确窗口 |
| correctness-qualification | c29aa8d5的独立clean构建/测试/安装资格，避免被最终构建覆盖 |
| q | C1最终Toolchain/Developer/Lua54；all、no-op、CTest、安装、编译和实际资产身份；qualification.json追加记录各轮身份 |
| final-cases | 两VM各五个独立实际分支、provider断言及EXE身份 |
| consumers-final / incremental-final | 15安装消费者、13项增量预期检查与生成产物 |
| scalar-trial / record-trial | 两个预定窄改的全部五对与独立保护/分配诊断；开发产物按真实哈希记录 |
| performance-C0-C1 | 最终八场景80次独立进程、完整同量工作与配对分布 |
| performance-H-C1-clean | 最终H→C1 scalar五对，独立新PowerShell进程无初始化警告 |
| performance-H-C1 | 首次H测量：上层VS环境初始化警告，保留但排除最终统计；原因见invalid-trials.json |
| record-final | H无保护、protected诊断、C0/C1正式输出全部配对；typed Pose绝对成本；Lua54保护/错误/寿命诊断 |

所有验证仅RelWithDebInfo。计时与分配/保护计数/故障注入分开；计数关闭的0不代表零分配。
旧benchmark缺失errors/failures保持null；provider负例由独立进程真实计数证明。最终成本未宣称等价或全部必要。
总时间、按真实调用/完成摊销成本、backlog、配对差和百分比完整记录，frames不当作独立样本。

固定归档提交 `f5c273384ef54a7a2b95d35c77077a305f1ced7e`：
[下载ZIP](https://github.com/LUX-YU/lux-engine/raw/f5c273384ef54a7a2b95d35c77077a305f1ced7e/.internal/evidence/script/sr5/admission/SR5-admission-raw-evidence.zip) ·
[查看文件索引](https://github.com/LUX-YU/lux-engine/blob/f5c273384ef54a7a2b95d35c77077a305f1ced7e/.internal/evidence/script/sr5/admission/raw-files.json)。

[remote-verification.json](remote-verification.json) 记录从全新bare仓库实际fetch远端后重新读取归档与索引，ZIP和全部1,363项文件哈希校验通过；未借用本地工作库对象。
后续文档提交不改变最终生产身份或归档内容。
