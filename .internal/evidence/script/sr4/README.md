# SR-4 新证据增量

实现与限制见[SR-4交付报告](../../../script-system-sr4-closeout-2026-09-07.zh-CN.md)，
完整所有权及旧断言映射见[协议记录](../../../script-system-sr4-implementation-2026-09-07.zh-CN.md)。
本目录只新增本轮证据，原SR-2/SR-3归档与提交身份不变。

## 固定下载与完整性

- 验证源码：`1750ce854967a382ee89accf3a3534a0628396cc`，独立 clean clone、RelWithDebInfo。
- 原始归档提交：`c01ca39c5333590bb928efa5ee8a1bed28ed8fcd`。
- 仓库内归档：[final/SR4-raw-evidence.zip](final/SR4-raw-evidence.zip)。
- [固定提交 ZIP 下载](https://github.com/LUX-YU/lux-engine/raw/c01ca39c5333590bb928efa5ee8a1bed28ed8fcd/.internal/evidence/script/sr4/final/SR4-raw-evidence.zip)。私有仓库访问沿用仓库身份认证。
- [固定提交逐文件索引](https://github.com/LUX-YU/lux-engine/blob/c01ca39c5333590bb928efa5ee8a1bed28ed8fcd/.internal/evidence/script/sr4/final/raw-files.json)。
- ZIP 5,475,290字节，1,743项，每项均有路径、尺寸、SHA-256；不含EXE/DLL/OBJ/PDB/LIB。
- ZIP SHA-256：`68f84e711e9ab8dbd52a54d8d294b79e6ff876a299049865c337297594db17e7`。
- [SHA256SUMS](final/SHA256SUMS)、[产物/安装身份](final/identity.json)、[实际安装链接闭包](final/installed-link-closure.json)、[远端重取验证](remote-verification.json)。

验证使用既有传输仓库 fetch 固定远端提交，再 git show 读取包、索引和SHA文件，核对ZIP和1743项内容。
不以本地同名包代替远端验证；下载后的本机绝对路径只作为执行身份，交付入口为上述固定提交。

## 快速阅读

[一页CSV](one-page.csv)给出两个参照、六场景的批量总时间、配对中位及范围；
[一页JSON](one-page.json)另含每对差值、有效工作量、backlog、实际预算、整批摊销ns/操作和冷期/两种规模。
保留旧errors/failures为null。Flow计时外INTEGRITY检查见原始log；其保留错误记录不是无界累计错误计数器。

| 归档内部路径 | 内容 |
|---|---|
| sr4/start-identity.json、final-identity-check.json、preserved-work-and-header-sync.json | 开工与最终依赖/源码、8项未知文件未变、公共模块头同步 |
| sr4/candidate/qualification.json、candidate/{t,d}/ | 最终source_sha=1750ce85的107/107及119/119、all构建、安装、no-op和完整CTest |
| sr4/initial-qualification/、sr4/capacity-qualification/ | 中间快照记录；不代替最终验证 |
| sr4/validation-final/consumers、consumers-driver.log | 14消费者生成/配置/构建/执行、实际link与安装闭包 |
| sr4/validation-final/probes/ | 6生命周期+8Event准确插桩/轨迹检查、wire288字节、冷期五对和独立分配 |
| sr4/validation-final/scale/ | 8/8192配置、16warmup、128次单实例重建，访问量/业务/错误/积压与分配诊断 |
| sr4/validation-final/flow-allocations/ | 同源码、匹配生成头、EXE-local分配诊断与产物SHA |
| sr4/diagnostics/lua-before/、lua-e0-six-*、lua-fix-* | 真实资产、编译宏、逐step挂起/完成/恢复、六项修前失败和修后通过 |
| sr4/diagnostics/timer-source-proof-final/ | 同一测试EXE下E0取消物理计数失败→E1通过、DLL身份 |
| sr4/diagnostics/layout-final/ | 实际编译器单类型layout、命令和尺寸；不在计时产物插桩 |
| sr4/final-e0-e1/、final-h0-e1/ | 两个独立同条件六场景×五配对矩阵、所有原始CSV/log/命令/身份/有效性 |
| sr4/observers-final/{e0,e1,h0}/ | 同源码Flow observer、匹配生成投影、EXE/DLL哈希；计时外INTEGRITY |
| sr4/cost-summary.json、frame-distributions.json、one-page.* | 批量/实际操作成本、配对分布与分开分类的帧p50/p95；帧不是独立重复 |
| sr4/invalid-trials.json、invalid-measure-build-root/及相关原log | 无效原因、部分有效子运行、不完整试验；未拼入最终矩阵 |

主要结论：E0→E1持续FlowEvent配对中位−2.27%，H0→E1仍+11.14%，旧债务未解释。
本轮LuaUpdate+1.52%（整批摊销+0.3912ns/实际调用）和首次prepare约+10微秒单列为新增、未独立归因成本。
没有宣称全局性能等价、全部DLL零分配或所有VM/平台资格通过。完整限制以主报告为准。
