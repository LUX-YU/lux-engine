# Lua55 v3 原始证据

Engine 资格源码：`e06208deb29b9b1de52fe49a129091f693bb7e08`。v2 实际镜像：`a6f16d6de6a67f6a4422553d31b42c1ac2b3e4c0`。
VM 实测源码 `5985cc4d49fba8aecfcb40fa9bdb70ea0204f55a` 与最终候选的 VM 生产输入相同（见 continuity）；不把文档提交当新测试。

[结果报告](../../../lua55-v3/RESULT.zh-CN.md) · [result.json](../../../lua55-v3/result.json)

[原始 ZIP](raw-logs.zip) · [SHA-256](raw-logs.zip.sha256) · [归档身份](archive.json)

MANIFEST.json 记录每项路径、大小、SHA-256；打包后已逐项读回核验。
没有 EXE/DLL/PDB/OBJ/LIB/bitcode 或 cooked 二进制。实际 matched image 在受控本地 lua55-v3/final-image 保留；
哈希在 final-image-manifest.json。完整原始 VTune 数据库留本地 r0-profile/result，归档保留其命令、退出码和导出报告。

| 内容 | ZIP 内入口 |
|---|---|
| 开工/固定依赖/未知修改 | r0-start.json；baseline-dependency-verification.json；fixed-dependency-additional-files.json；protected-workspace-final.json |
| 最终 clean clone 和构建/测试 | git-final-integrity.json；qualified-tracked.*；qualified-d-*；qualified-t-*；configuration/ |
| Lua55 正式与独立诊断合同 | final-vm-*；diagnostic-vm-*；vm-source-continuity.json；vm-patched-inputs.json；vm-source-manifest.json |
| R0 软件 VTune | r0-profile/runs.json、target-exit.json、target.log、hotspots.csv、top-down.csv、modules.csv |
| 真实机器码 | machine-code/commands.json、rvas.json、*-uf.log；sequence-frame-before/after.log |
| R1 allocator / R2 local route / R4 codec | r1-*、r2-routes-*、r4-codec-*、integrated-protocols-*、generated-d/t-tests.log |
| 实际 SDK 与 15 consumers | installed/consumers.json；installed/*/configure/build/run/second-build.log；installed-final-manifest.json |
| 两条迁址执行 | relocated/results.json；relocated/unavailable-paths.json；relocated/*/run.log；relocated-qualified-source-driver.log |
| 13 类生成增量 | value-incremental/probes.json、每次 generated.hpp、log |
| 五腿三对整体成本 | final-costs/identities.json、runs.json、summary.json、work-oracles.json、各次 CSV/log |
| 独立内存 | memory-diagnostic/results.json、analysis.json、两侧 CSV/log |
| 修前失败与修正 | failed-style/；failed-integration/；sequence-diagnostic-*；sequence-frame-comparison.json；sequence-fixed-* |
| 迁址环境限制 | relocation-initial-limitation.json；relocated-driver.log；首次开发克隆 rename 被拒，已原子恢复 |

阶段命令里的历史 source 身份保留原值。最终引擎资格使用 qualified-*；不能把前面的格式/错误 profile/新 frame 超容量失败当成通过。
正式 VM counters OFF 的原始零输出不解释为“零工作”；解释后的 result.json/analysis.json 使用 null。
计时只含 30 个有效 AB/BA/AB 进程，不含 profile、内存或 debugger。
迁址隐藏了实际资格源码/两构建树/原 SDK；开发克隆因占用保持可见，不能声称所有源码都消失。

固定归档提交：`ca3b891ccda8d8608fa225e9f8899915c4bb2d05`。
[固定提交下载 ZIP](https://raw.githubusercontent.com/LUX-YU/lux-engine/ca3b891ccda8d8608fa225e9f8899915c4bb2d05/.internal/evidence/script/lua55-v3/raw-logs.zip) · [远端下载及逐项哈希验证](download-verification.json)。
归档已从远端重新取得，SHA-256 与全部 479 项文件一致；未重打包。
