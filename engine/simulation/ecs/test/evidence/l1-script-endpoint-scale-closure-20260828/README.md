# L1 Script / Hook / Event 性能收敛资格证据

## 结论

本记录验证 production commit
`588a280698b451c49c72884f0d5ee01ab206b4ad`。构建、测试、安装消费端、架构门禁和
benchmark v10 均通过。本提交只记录资格证据，不改变生产代码；在独立 API/语义审查完成前，
本轮状态为 Freeze Candidate，不在本记录中宣告正式 FROZEN。

## 契约与提交身份

- 分支：`codex/l1-script-endpoint-scale-closure`
- production commit：`588a280698b451c49c72884f0d5ee01ab206b4ad`
- production parent：`5ccf2a795d757da775f6a2100f2380da5b7f3b11`
- production tree：`7c11d40c3c75c20483166b8e59a8ec234d9e67af`
- 原始实施规范 SHA-256：
  `6638A15114C228CEF9E23F3E37BD2F71CB8FFFE7E2805CAC556B52F2BAF100E5`
- 仓库内换行规范化后的规范 SHA-256：
  `1F65F5F4F7EB0DBEC90F6FDFC5805EE3D89B4F4BD11C0B84B57EF0B0240FAE41`
- exact-SHA detached worktree：
  `E:/SyncForder/CodeRepos/worktrees/lux-engine-l1-script-endpoint-scale-closure-verify`
- 验证时 implementation worktree 与 detached worktree 均为 clean。
- 原始 `main` worktree 的两项未提交用户修改未被暂存或覆盖。

`588a2806` 是 exact Android 验证发现后的 production correction：
`SimulationStepInfo.hpp` 公开使用 Script semantic 类型，因此
`simulation_description` 公开导出 `script_core` target 与安装包依赖。

## 工具链

- Git `2.48.1.windows.1`
- CMake `4.1.2`
- Ninja `1.11.1`
- MSVC `19.44.35228` x64
- Android NDK `30.0.15729638`
- Android Clang `21.0.0`, `arm64-v8a`, API 33
- MLIR/LLVM `18.1`
- Python `3.12.13`

所有桌面构建均从 Visual Studio Developer environment 执行，使用
`all -- -j 4 -k 0`。每个 CMake 配置的第二次构建均报告
`ninja: no work to do.`。

## exact-SHA 构建与测试矩阵

| 配置 | 结果 | CTest |
| --- | --- | ---: |
| RelWithDebInfo / DEVELOPER | PASS | 91/91 |
| Debug / DEVELOPER | PASS | 96/96 |
| RelWithDebInfo / DEVELOPER / Object+UI hardened | PASS | 96/96 |
| RelWithDebInfo / TOOLCHAIN / FlowForge+MLIR | PASS | 74/74 |
| RelWithDebInfo / Android arm64 PLAYER / NDK30 | PASS | cross build, tests not executed |

仓库没有独立的 Simulation hardened CMake 开关；Simulation 合同由 source architecture
gate、L1 negative tests 和对应 runtime tests 覆盖。

Android PLAYER 的构建图检查确认无 Lua、FlowForge、MLIR 或 LLVM library/target
进入产品闭包。NDK 自身使用的 LLVM 编译工具不属于产品链接闭包。

## 安装与消费端

- exact DEVELOPER runtime fresh install：
  `E:/SyncForder/CodeRepos/build/L1ScriptEndpointScaleClosure/exact-5ccf/runtime-install-final-588`
- runtime installed-architecture scan：PASS
- exact TOOLCHAIN fresh install：
  `E:/SyncForder/CodeRepos/build/L1ScriptEndpointScaleClosure/exact-5ccf/toolchain-install-final-588`
- 11 个 fresh installed consumers：全部 configure/build/run PASS
- 11 个 consumers 的第二次构建：全部 `ninja: no work to do.`

通过的 consumers：`core-system`、`core-task`、`script-binding-authoring`、
`simulation-asset`、`simulation-description`、`simulation-ecs`、
`simulation-system`、`snapshot`、`system-hook-script-binding`、`world`、
`world-asset`。

Toolchain SDK 按设计包含 MLIR/LLVM 头与库，通用 Runtime/Player 安装面扫描不适用于该
SDK；该扫描在独立的 DEVELOPER runtime fresh install 上执行并通过。Toolchain SDK 由
TOOLCHAIN 构建、74 项 CTest 和 11 个安装消费端验证。

## 架构门禁

source architecture report：

```text
vNext L1 semantic architecture debt: 0
legacy roots configured: 0
legacy includes from production: 0
retired top-level ECS domain/namespace: 0
L1 terminal I/O: 0
retired L0 asset runtime vocabulary: 0
transform full-scan/associative dirty paths: 0
legacy entries in compile_commands: 0
configuration serialization includes in Engine/L1: 0
component codecs/runtime-reflection persistence: 0
binary serialization runtime-reflection closure: 0
```

report SHA-256：
`62FABAEE143412795BFBAEC80DE5624A07688135F2050CA55EC7A1AAA477A43D`。

## benchmark v10

正式 performance 模式使用每用例 5 次 warmup、30 个 samples。输入覆盖：

- Hook 1M dense handlers 及随机 disconnect/reconnect；
- Entity-targeted Event 100K、1M、2M sparse subscribers；
- 1M command-buffer records；
- 100K/1M reactive dirty；
- 1M real worker-owned event buffer；
- 1M bindings 中精确 detach 四个 bindings；
- 100K distinct Script dirty marks；
- prepared C++ call 与 global Event。

evaluator 结果：

```text
PASS: 2 scaling, 1 ratio, 27 structural rules
```

benchmark CSV 均记录 exact production SHA
`588a280698b451c49c72884f0d5ee01ab206b4ad`。

关键产物 SHA-256：

- `ecs_l1_benchmark.exe`：
  `4C87700EE70D2FAA9C6F8797477C2CAC1582FDC196A2AB3BCB972535EA883A3D`
- benchmark policy：
  `04A9BCE304C6C0238B6685C879C4F72B1C09D6018F091322546B936EBB32FE3A`
- benchmark evaluator：
  `7B0EC294B7920F48B6EA5B6479D8E415DF13D3F01BCCF80553A4E20E5CD1621E`
- Android `liblux_engine_simulation_script.so`：
  `12F71C5A96641F925C0F6515CC9F186272D1DAF4A0B91F42119B550B9CFE0DD4`
- FlowForge compiler DLL：
  `351D4F7194D88D309514D97CCD208C5D7572D439EE79F34C74A4A308DE77A176`

## 验收边界

本证据证明本轮实现满足内部 build/test/install/architecture/performance qualification。
它不替代独立代码、API 和语义审查，也不自动解除后续层级的外部验收门禁。
