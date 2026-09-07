# SR-3 代码与原始证据

最终生产代码：`3e2bcbd29580d2e744744b64149b7d8fedcc8950`；修正 SR-2 参照：
`8145598c18421d03da1dac21251200af3290e27d`。完整设计、断言映射、实际结果和限制见
[主文档 §13—14](../../../script-system-sr1-design-2026-09-06.zh-CN.md)。

- [最终原始包](final/SR3-raw-evidence.zip)：1,501 个文件，2,930,879 bytes。
  [逐文件索引](final/raw-files.json)、[SHA-256](final/SHA256SUMS)、[安装身份](final/identity.json)、
  [实际链接闭包](final/installed-link-closure.json)、[结果汇总](final/results.json)、
  [额外 FlowForge 业务行校验](final/additional-checks.json)。
- [迭代原始包](iterations/SR3-raw-evidence.zip)：2,118 个文件，3,999,269 bytes。
  [逐文件索引](iterations/raw-files.json)、[SHA-256](iterations/SHA256SUMS)、
  [当时的源码及安装身份](iterations/identity.json)。保留 0cf053b9/6ac051dd 的构建与测量、
  单配置预检优化前结果、初版热路径回退、重入测试迭代、同步退休错误修前失败及驱动失败。
- [原 SR-2 包](../sr2/README.md)和[SR-2 修正门槛包](../sr3-gate/README.md)保持不变。

最终包 SHA-256：`4fb6bb7ce42de5a78c818d9bf4832d786ced31c57569c9431f0b11a08e721a71`。
迭代包 SHA-256：`74f7e0180be1efc02c6060839e8f83ea58900ade14ba61eba881391f4e9fe0ef`。
包内只包含原始日志、命令/配置、诊断源码、CSV、哈希身份与 wire golden，不含编译二进制。

资格是独立 clone、RelWithDebInfo、全量 `all -j 4 -- -k 0`、串行构建/测试及安装。
6ac051dd 从零构建后，在同一 clean clone 的 3e2bcbd2 上重新配置、构建及全量复核；前次日志
已保存到迭代包，最终包保存后次日志。qualification.json 保留两次提交的记录，应按 source_sha
选择，不把历史记录当作新通过。

最终 Toolchain 107/107、Developer 113/119、installed consumers 14/14；Developer 仍为参照的
相同六项 Scene Lua 断言失败。六组 lifecycle、八组 Event 的 CASE/计数及完整轨迹相等，wire v1
为固定 288 bytes。72 个运行时测量进程、24 个规模进程及冷期/FlowForge 分配诊断均已完成。

**性能没有达到等价**：完整工作量的五对中位增幅为 C++ update +68.90%、FlowForge update
+25.36%，其余路径见主文档。8,192 配置单实例重建中位耗时降低 72.28%，两种规模均只有
128 个预检槽位、0 个端点目录计数复制。所有计时及业务行都保留，未以调整工作量或预算消除成本。
EXE-local new 不覆盖全部 DLL 堆分配；FlowForge 补充诊断的 CSV stamp 为 unknown，身份由
其源码、qualified native projection、RelWithDebInfo 构建命令及 EXE 哈希关联。

固定提交下载入口：

- [最终 ZIP @ 07488c1e](https://github.com/LUX-YU/lux-engine/raw/07488c1eb8491e57bdfe9a0973e1a9682bac1de8/.internal/evidence/script/sr3/final/SR3-raw-evidence.zip)
- [迭代 ZIP @ 07488c1e](https://github.com/LUX-YU/lux-engine/raw/07488c1eb8491e57bdfe9a0973e1a9682bac1de8/.internal/evidence/script/sr3/iterations/SR3-raw-evidence.zip)

从 GitHub 经 SSH 新建独立 clone，按固定提交取回上述两包及原 SR-2、修正门槛包：四个 ZIP
完整性和共 4,381 项文件哈希全部匹配，无编译二进制。详见[远端取回记录](remote-verification.json)。
原 SR-2 索引中的 ee80710d 固定入口及原 production identity 保持不变。

本阶段停止于 SR-3，等待独立审阅。
