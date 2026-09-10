# ExecutionCell A1 原始证据

本包只对应实验分支 `codex/s6-execution-cell-a1`。A 为 `f7d2815bdd2025ee23a7c11449def822413f58e9`；
B 实际完整资格/计时为 `caf34bd307912450d894da922b59af4238154508`。归档/报告提交不作为新测试身份。

[结果报告](../../../script-cell-a1/RESULT.zh-CN.md) · [实际数据](../../../script-cell-a1/result.json) ·
[合同映射](../../../script-cell-a1/TEST_MATRIX.csv) · [原始ZIP](script-cell-a1-raw.zip) · [SHA-256](SHA256SUMS)

归档 `5311142` B，`557` 项文件 hash、`558` 项ZIP entries，已逐项核验。
SHA-256：`af17674b63690ba77b48aefd63b5e54d188d592dc33151a3bb0e23d63a6aef98`。无DLL/PDB/EXE/OBJ产品二进制；匹配镜像保持在原本地目录，身份清单入包。

- `final-*-*.log/json`：clean clone all/no-work/CTest/install与命令退出码；`final-*-LastTest.log`保留实际断言输出。
- `final-costs/`：全部42个有效正式进程、逐帧CSV、原始stdout、命令、批次分位和配对分布；无性能重测替换。
- `resource-diagnostic/`、`layout/`：独立cell/VM观察、内存请求及类型布局，未混入正式计时。
- `contract-comparison/`、`additional-contracts/`：A/B固定DLL上的同一真实fixture业务轨迹，测试源码hash和增强输入保留。
- `installed/`、`value-incremental/`、`relocated/`：15消费者、13增量与2迁址链。
- `candidate-profile/`：一次B软件采样的原始VTune记录、业务、导出表及命令；`candidate-script-disassembly.log`为实际DLL机器码。
- `*attempt*`及早期build/test日志：失败与驱动拒绝原因原样保留，不冒充有效资格。
- `final-audit.json`：204依赖、镜像/资产/安装头、未知修改保护复核；`qualified-source.patch`仅便于查看已提交源码变化。

固定提交下载入口在归档提交产生后登记；本ZIP只打包一次，不改写旧v4归档。
