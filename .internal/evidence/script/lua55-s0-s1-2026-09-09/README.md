# S0/S1 Lua55 原始证据

状态：`CORRECTNESS_QUALIFIED_COST_OPEN`。仅首批 S0/S1，等待独立审阅；不批准产品默认 VM 切换。

[实施与限制报告](../../../script-lua55-s0-s1-2026-09-09.zh-CN.md) · [阶段结果](stage_result.json) · [完整配对表](cost-summary.md) · [机器可读成本](cost-summary.json) · [分配与采样摘要](diagnostic-summary.json)

最终资格源码：`55dcb3fa85a9a2b7dcc87b178dd9ff76ba75c644`；生产实现与 `2e915205` 相同。
S0 为 `aabbabbca5a02b535a608e8d43594fe448a48cdf`，同 VM 修正参照为 `c72d88ce4c17a5f81f85e278610a3f987d4b828d`。文档/归档提交不重标这些身份。

| 归档 | 字节 | 原始文件数 | 内容 |
|---|---:|---:|---|
| [qualification.zip](qualification.zip) | 2778735 | 452 | 所有阶段原始 build/CTest、修前修后负例、断言映射与独立诊断 |
| [identities.zip](identities.zip) | 606303 | 42 | 固定依赖、原/候选二进制哈希、CMakeCache 与编译命令 |
| [installation.zip](installation.zip) | 732391 | 606 | 15 消费者、最终迁址、13 增量、VM 错配与 C close-error 探针 |
| [costs.zip](costs.zip) | 8656188 | 665 | 五对时序 CSV/日志、内存、VTune 导出、实际 CRT 反汇编 |
| [vtune.zip](vtune.zip) | 2858362 | 72 | 两次原始 VTune result/data/sqlite；首次缺少目标日志而无效，captured 为有效采集 |

[归档 SHA-256](SHA256SUMS) · [归档清单](archives.json) · [逐文件 SHA-256](raw-files.json)

归档按原始相对路径解压到同一目录。已逐条读取并验证 1837 个条目；本目录的 `.gitattributes` 保留精确字节，防止 checkout 的换行转换破坏哈希。
EXE/DLL/PDB 不在归档中；本机保全产物位于 `E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-lua55-s0-s1-20260909/images`。VTune 重新定位符号时使用对应哈希的本机产物；不能仅凭缺少 PDB 的系统函数近似名推断根因。

验收入口：

- 最终三 VM Developer 各 128/128；Toolchain 109/109：`s1-final/`、`s1-final-jit/`。
- 15 消费者绑定 `2e915205`；最终 `55dcb3fa` 的两条迁址链与 13 个增量检查位于 `relocated/`。
- 真正失败注入：`creation-branches-calibrated/`；三 VM 函数/版本分支映射：`final-mapping/assertion-map.csv`。
- Lua55 有效采样：`profile55-captured/target.log`、`identity.json`、`top-down.csv`、`result/`；首次 `profile55` 保留无效状态。
- 29 个完整五对集合，另保留 CLI cold 失败和 Physics 四对不完整组。原 Physics exit 1 原因未解释，debugger exit 0 与新五对不能证明原失败已修复。
- Lua54 A/A 失败、同 VM 安全修正 +3.00475% 确认结果、Lua55 较 LuaJIT 明显更慢都保留，不授予性能等价。
- 批次 p99 不代表逐 continuation 恢复延迟；缺少累计错误导出的场景保持 null。内存读数不代表关闭后的零泄漏。

[复现驱动](../../../diagnostics/lua55-s0-s1-2026-09-09/) 与 [Lua55 独立 C 依赖构建说明](../../../../cmake/dependencies/lua55/README.md) 随源码交付。仅 RelWithDebInfo，构建/运行串行；不重打包任何旧阶段归档。

归档后仅修正阶段 JSON 校验器的 Python 包假设，未更改测量或归档；见 [补充校验记录](postarchive-validation.json)。
