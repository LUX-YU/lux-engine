# 脚本主线合并记录（2026-09-12）

用户在清理结果和剩余成本评估后明确授权“合并到主线并推送”。本次授权更新此前的禁止合并停点；
保留性能债务，不授予性能等价，也不关闭范围外的 Editor ER-1、GPU 或其他引擎工作。

## 源码与隔离

- 主线父提交：`f89e186216ca904055a6be7ffdccabaae3befff1`。
- 脚本父提交：`70167bb3aaaa498cae682e5e1da502e64f6759d3`。
- 合并提交：`43d22937f50fdf6d9716d47dd97100cf13a05055`，普通双父合并，无文本冲突。
- 本轮实际资格源码：`dcc752759a30f82c10e0188fc3e024110b506338`。后续文档不是新资格身份。
- 使用独立、干净的 qualification clone，先通过 ValidateTrackedSnapshot；复用两个受控构建槽位。
- 固定 lux-cxx `3100f54d`、toolset `99c3d048` 和实际安装的 204 个文件哈希；Lua 5.5.1、
  既有 VM patch、INC、16 MiB、Native ABI6 不变。三个公共头安装前缀的 24 项比较全部已匹配。

主线原有 7 项修改先逐文件备份和记录哈希，不纳入提交。两份 Script 头的本地改动仅为排版，
在新签名上重放该排版，保持候选的非空白内容；其他原文件保留原字节。
已从正式源码删除的 `cmake/RunScriptV3Measurements.ps1` 在本地保留为未跟踪文件，
原有 `RunScriptV3Stress.ps1` 也保持未跟踪，不恢复旧 VM 的正式驱动。
原始备份位于本机 `script-native-lua-na1/main-merge-20260912/original-main/`，不上传未知修改内容。

## 合并期间的最小修正

第一次 Toolchain configure 被已有目录检查拒绝：Editor `application/tooling` 与 `ui/scene`
是 ER-1 已采用的独立包，但通用规则只允许父目录为纯集合。相关目录与旧检查在主线父提交中已存在，
不是脚本合并新增的运行时回退。

`dcc75275` 只协调构建检查：这两个精确路径仍由 `engine/editor/CMakeLists.txt` 配置，
禁止其外围包自行聚合；未登记的其他嵌套继续拒绝。原目标分类、DAG 和 ER-1 职责检查保留。
新增测试覆盖两条合法路径、未登记子包、父包聚合和缺失集合 owner，共五个断言场景。
没有修改 Editor 或脚本运行时实现，没有为通过构建删除检查。

## 新候选验证

仅 Windows / MSVC 19.44 / RelWithDebInfo，构建与测试串行。

| 验证 | 实际结果 |
|---|---|
| Toolchain all `-j 4 -- -k 0`、第二轮无工作、全量 CTest | 111/111，43.91 s |
| Developer all `-j 4 -- -k 0`、第二轮无工作、全量 CTest | 133/133，42.86 s |
| 固定 Lua55 VM 合同 | 4/4 |
| 新 SDK 的原 16 个消费者，重新生成/编译/运行/第二轮无工作 | 16/16 |
| 值转换生成增量与负例 | 13 类通过 |
| 步骤/任务/路由等负例、恢复执行与 no-op | 5 类通过 |
| 新位置的 Lua values、Lua packager、Native Lua tasks | 3/3 |

迁址期间资格源码、两套 build 和原新 SDK/tools 安装目录实际不可用，测试后全部恢复。
新 SDK 使用 `install/o/na1/main-merge-sdk` 和 `main-merge-tools`，不覆盖旧资格 SDK。
保留首次 configure 失败日志；路径长度等 CMake 警告仍在原始记录中，实际 all 与执行结果如上。

原始命令、退出码、完整日志、生成检查、依赖和产物哈希见
[本轮证据](../evidence/script/main-merge-20260912/INDEX.md)与
[机器可读结果](../evidence/script/main-merge-20260912/result.json)。

## 保留限制

本轮没有重新采集性能。`e39535cf` 的
[清理成本记录](CLEANUP-RESULT.zh-CN.md)继续使用原身份：32 次 Event 完整任务配对中位 +1.57%，
FlowForge Event +1.76%，均未解释；既有 scalar/coroutine/Native metadata 债务也保留。
这些数据不是合并候选的新测量，不用其他场景收益抵消差异。

没有验证本合并提交的 Android 或 Editor GUI/GPU 资格；不改写既有 ER-1 未完成状态。
本轮执行普通主线合并与推送，不强推、不发布 tag、不删除实验分支或历史证据。
