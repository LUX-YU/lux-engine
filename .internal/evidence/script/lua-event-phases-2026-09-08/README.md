# Lua Event 逐阶段成本证据

[分析报告](../../../script-lua-event-phase-investigation-2026-09-08.zh-CN.md) ·
[诊断与重放](../../../diagnostics/lua-event-phases-2026-09-08/README.md)

- [原始归档](lua-event-phases-raw.zip)：6,661,333 bytes，265 项原始文件和一份内嵌逐文件哈希清单。
- [SHA256SUMS](SHA256SUMS)：`816d7baef2c298491fd10477abfa47882032f0e43e05f1147b5acf11c26788ff`。
- [归档摘要](archive-summary.json)。打包后已重新读取每项内容校验全部哈希。
- 固定归档提交：`d299542950f1cc7c2db808b3e427f71b8fba2505`。
- [固定提交浏览入口](https://github.com/LUX-YU/lux-engine/tree/d299542950f1cc7c2db808b3e427f71b8fba2505/.internal/evidence/script/lua-event-phases-2026-09-08)
  与 [固定提交下载入口](https://github.com/LUX-YU/lux-engine/raw/d299542950f1cc7c2db808b3e427f71b8fba2505/.internal/evidence/script/lua-event-phases-2026-09-08/lua-event-phases-raw.zip)。需要仓库访问权限。

生产源码是 `bc2dbfe2270a4b6777207deceb1ad33b78a02920`，与本轮分支起点 `6dcbe656` 的生产目录一致。
`d2995429` 只增加诊断/证据，不是另一份已测生产候选。实际 core DLL 经过同源码重新链接，
应使用 `identity-final.json` 的哈希，不能使用 `identity-start.json` 的旧图像哈希替代。

| ZIP 内入口 | 内容 |
|---|---|
| `identity-start.json`、`identity-final.json`、`probe-identity.json` | 源码、实际安装依赖、保护文件、Lua artifact、EXE/DLL/PDB/编译身份 |
| `runs.json`、`*.log`、`*.command.json` | 实际 argv、退出码、独立 application integrity、编译/链接命令 |
| `timing-0..4-original.csv`、`timing-0..4-phases.csv` | 五对全部批次；观测器对照，不是优化对照 |
| `memory-0.csv`、`memory-1.csv` | VM accounting 开启的两轮独立分配诊断 |
| `roi-1/`、`roi-2/` | 两轮有效 VTune 原始项目；可由 VTune 打开 |
| `roi-*-tasks.csv`、`roi-*-<phase>-top-down.csv` | ITT 阶段 CPU 及各阶段 inclusive 调用栈 |
| `roi-*-physical.csv`、`roi-*-hotspots.csv`、`roi-*-callstacks.csv` | 同批样本的物理/内联/完整栈不同呈现，禁止相加 |
| `assembly-*.csv` | `active`、`waitEvent`、`takeAwaitable`、`resumeOne`、结果移动、`lua_newthread` 实际汇编 |
| `analysis.json` | 从原始 CSV 导出的配对、尾部、分配和采样占比 |
| `trial-disposition.json`、`roi-0/`、`hardware.log`、`compile.log` | 被排除的运行与原因，不删除失败试验 |
| `all.log`、`all-second.log`、`affected-tests.log` | 全量构建、第二轮无工作、16/16 受影响测试 |
| `raw-files.json` | 265 项文件大小和 SHA-256 |

构建实际命令：`cmake --build E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w/d --target all -j 4 -- -k 0`。
测试实际命令：`ctest --test-dir E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w/d -C RelWithDebInfo --output-on-failure -R "simulation_script_(lua|event|continuation|lifecycle|bindings)"`。

本地完整产物位于 `E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-lua-event-phases-20260908`。
`images/` 保留本轮 EXE/DLL/PDB，编译图像不放入仓库归档；原始 VTune 数据不是编译二进制。
迁址重新解析符号时须选择记录的匹配图像，不使用同名但不同身份的 DLL。

只有 Windows/LuaJIT 的本负载新证据；硬件 PMU、Lua54、多线程竞争、独立字段读回、每个 waiter 的
延迟分布、OS RSS/峰值内存不在本轮测量范围。旧资格、支持矩阵和债务继续引用主报告中的既有入口。
