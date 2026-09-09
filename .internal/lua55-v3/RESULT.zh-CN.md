# Lua55 v3 实施记录

状态：实施中，尚未完成联合资格。参考提交 `10a70e80e6e2879ecb09f14ffc083516d0666752`；
性能参照是 v2 实际资格 `a6f16d6de6a67f6a4422553d31b42c1ac2b3e4c0`，不是文档提交。
开发使用现有隔离克隆 `build/RelWithDebInfo/s5/source`，原始 main 的七项未知修改不纳入本轮。
Lua55 是唯一活动 VM；不重跑 Lua54/LuaJIT。INC、上游 GC 参数、Native ABI 6 保持。

## R0

复用并核对 `build/RelWithDebInfo/lua55-v2/final-image` 的 49 项匹配 EXE/DLL/PDB/资产，
共 255118963 字节；不重复创建完整构建树。启动身份和未知修改哈希在 `lua55-v3/r0-start.json`。
固定依赖沿用 v2 已验证的安装 cxx `3100f54d`、toolset `99c3d048`，不采用源仓库 HEAD。

一次 Event 软件 VTune 采集已通过计时外全实例值和关闭校验，真实完成 30000000 次，
每实例 93001，checksum 930010000，错误及最终 backlog 为零。
采样范围包含启动、预热、业务、oracle 和关闭；只过滤目标进程，不用采样时间作为计时结果。
当前 self 样本：`luaH_Hgetshortstr` 3.137 s、`malloc_base` 1.869 s、`sweeplist` 1.585 s、
`luaV_execute` 1.370 s、旧 allocator resize/acquire 合计 2.342 s、`free_base` 0.594 s、
`freeCI` 0.448 s。整个 VCRUNTIME140 模块仅 0.152 s，不能继续引用旧 longjmp 26.3%。
原始命令、进程退出、CSV 与报告在 `lua55-v3/r0-profile`，软件采样未给出 PMU 结论。

R1–R4 连续实施；R5 按当前浅调用事实决定是否选择；最终才交付统一结果。
