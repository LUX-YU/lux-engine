# Lua55 v2 原始证据

资格源码：`a6f16d6de6a67f6a4422553d31b42c1ac2b3e4c0`。文档提交不产生新测试身份。

[原始记录 ZIP](raw-logs.zip) · [SHA-256](raw-logs.zip.sha256) · [归档身份](archive.json)

归档内 MANIFEST.json 逐项记录路径、字节数和 SHA-256；打包后已逐项重新读取核验。
没有 EXE、DLL、PDB、LIB、OBJ 或 bitcode 产品。最终产品与 PDB 在受控本地 final-image 保留，哈希在包内 final-image-manifest.json。

| 内容 | ZIP 内入口 |
|---|---|
| 开工、固定依赖与工作区 | start.json；fixed-dependencies-audit.json；fixed-dependency-additional-files.json；protected-workspace-final.json |
| 最终 Developer / Toolchain | qualified-d-*；qualified-t-*；qualified-tracked.* |
| 最终 VM r2 | p8-vm-r2-linkage-*；vm-final-noop.log；installed-final-manifest.json |
| 旧接口拒绝 | legacy-selector-rejected.*；legacy-selector-restore.log |
| 当前 SDK | installed/closure-consumers.json；installed/*/closure-*.log |
| 迁址 SDK 实际执行 | relocated/results.json；relocated/unavailable-paths.json；relocated/*/run.log |
| 值生成增量 | value-incremental/probes.json 与各次 generated.hpp/log |
| 冷期容量修前/修后 | p7-reload-before-*；qualified-d-LastTest.log 的 COLD_CONTENT/COLD_RELOAD |
| 整体三轮成本 | final-costs/identities.json、runs.json、summary.json、work-oracles.json 与各次 CSV/log |
| 独立内存诊断 | memory-diagnostic/results.json 与两侧 CSV/log |
| 一次 GC / compiler 对照 | p3-gc-msvc/runs.json；p6-clang-inc/runs.json |
| 静态真实 FlowForge 尝试 | flow-bitcode.patch；static-provider.cpp；static-link.ps1；p8-static-* |
| 上游测试适配 | upstream-windows-devices-final.patch；p8-vm-r2-linkage-tests.log |
| 无效迁址与恢复 | relocated*-driver.log；partial-relocation-restored.json；relocation-tool-limit.json |

早期失败、未采用实验和原始退出码均保留。`final-*` 是收尾复核前的 0333ef0f 一轮，最终资格必须使用 `qualified-*`；
`closure-*` 包含扩展头 C++ linkage 修前失败，不能当作最终通过记录。
