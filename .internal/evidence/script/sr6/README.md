# SR-6 新增原始资格与成本证据

本包只属于本轮SR-6。原SR-2～SR-5包未改写；不将旧记录重标为本轮复跑。

- 资格源码F：`7b5e1dd4f824e0a2746b1dbb0ec2d0dcc57fe490`，独立clean clone，RelWithDebInfo。
- 生产参照E：`f64caddebcc353c07ac03637135f99586986eb6e`；同量成本复用身份已固定的匹配产物。
- lux-cxx：`3100f54d0743c5ed94a4ccf5943df04e933de255`；toolset：`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`。
- 归档：[SR6-qualification-raw-evidence.zip](SR6-qualification-raw-evidence.zip)，8,072,323 bytes、1,488项。
- SHA-256：`624c592916c9e32b0f81aff7c9048ddd56008759f0d91fb2c506141a447a26d4`。
- [逐项路径/大小/哈希](raw-files.json)、[SHA256SUMS](SHA256SUMS)、[源码/SDK/生成器身份](identity.json)、[实际安装链接闭包](installed-link-closure.json)。

所有1,488项已本地解包逐项校验。无EXE/DLL/LIB/OBJ/PDB；唯一wire二进制是288字节golden数据。
新的SR6未发布包使用既有collector后补入32项生成快照/诊断源码，最终哈希以上列为准，未触碰既有已发布包。
固定归档提交：`10f69062cbd04fbeb6ff9939362009e7f794a2f8`。
[固定归档下载](https://github.com/LUX-YU/lux-engine/raw/10f69062cbd04fbeb6ff9939362009e7f794a2f8/.internal/evidence/script/sr6/SR6-qualification-raw-evidence.zip)、
[固定逐项索引](https://github.com/LUX-YU/lux-engine/blob/10f69062cbd04fbeb6ff9939362009e7f794a2f8/.internal/evidence/script/sr6/raw-files.json)。
已使用新的bare仓库从GitHub取回该提交，重新读取归档和索引，1,488项大小及哈希全部匹配；
[远端核验记录](remote-verification.json)在包外，避免用本地包冒充远端下载或循环重写归档身份。

| 归档内路径 | 实际范围 |
|---|---|
| evidence-stage/start-identity.json、final-identity.json | HEAD/工作区/固定依赖，入口427个EXE/DLL未变、F产物/VM身份 |
| q/qualification.json、q/{t,d,l} | clean tracked gate、configure、all-j4-k0、CTest、install与no-op；108/123/110分别通过 |
| consumers-final/consumers.json | 15个原安装消费者；script-lua-values增加真实provider/lifecycle/recovery闭环 |
| incremental-final/probes.json；diagnostics/snapshots/incremental-final | 13项增量正/负检查及实际生成快照，原断言保留 |
| relocation/relocation.json、configure.log、build/ | 新SDK位置，旧源码/build/SDK隔离，公开依赖解析和重新生成/编译/执行 |
| audit/formal-paths.json | 七owner、C01—C13、正式入口/旧入口/Execution不含Lua转换状态的有限复核 |
| probes/manifest.json、baseline/、candidate/ | 6生命周期/8Event真实轨迹、命中/业务计数、wire golden、冷期和退休五对 |
| scale/runs.json、各CSV日志 | 8与8192配置、16warmup/128次只重建一实例、地址/槽位访问/固定存储和独立分配诊断 |
| protocol-E/、protocol-F/ | READY step0→实际resume step1..6，预算3，17个结果73、Channel reset、shutdown前业务drain |
| regions/ | 完整worker/owner窗口排他，21/82次、violations=0；原双Simulation和失败断言 |
| performance/runs.json、validated-costs.json、各CSV/LOG | 八场景五对80个进程，完整工作量/产物/参数/INTEGRITY；缺失旧错误字段保留null |
| record-costs/ | 两字段输出五对1M、typed Pose五对100k；VM分配和protected C调用分开诊断 |
| extra-costs/ | 停产后合法10步drain；两worker真实Script Hook数值集成；每进程内存峰值 |
| summary/costs.json | 严格E/F逐行同量核对、绝对/单位/配对成本、每run/帧类别分位数和内存 |
| memory-consumer-2/；evidence-stage/diagnostics | 公开安装纵向链的typed slots/frame上界；实际诊断源码和生成头 |
| consumer-development/、memory-consumer/；evidence-stage/invalid-trials.json | 两项无效开发尝试及原因；没有剔除有效慢性能样本 |

全部新命令/重放脚本在evidence-stage（qualification、post-qualification、relocate、measure-extra、record-costs、summarize等），
诊断生成的实际源码/头在diagnostics及evidence-stage/diagnostics。对应仓库内驱动固定于F；路径参数按新机器改写，
不能仅拿归档的绝对build路径运行旧EXE。所有新构建只用RelWithDebInfo，构建/测试/正式计时串行。

资格结论及限制见[一页汇总](../../../script-system-sr6-summary-2026-09-07.zh-CN.md)与
[详细报告](../../../script-system-sr6-qualification-2026-09-07.zh-CN.md)。
IMPLEMENTATION/CORRECTNESS/CODEGEN_INSTALL在列明范围通过；性能不授予统一PASS，历史债务不重标，其他环境未验证。
