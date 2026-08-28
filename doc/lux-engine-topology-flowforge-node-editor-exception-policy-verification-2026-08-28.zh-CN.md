# Source Topology / FlowForge / Node Editor / Exception Policy 验证证据（2026-08-28）

## 被验证提交

- 实施基线：`fb149b8f399e5c58f7eabe8a299829d873f169f1`
- 被验证的生产提交：`8ce0692212386796090cc3df8aae6e463670adac`
- 实施分支：`codex/topology-flowforge-node-editor-exception-policy`
- 规范 SHA-256：`1B175373BAF8EE9261DD4B7A23C71A8469E929D3559D2839EA107A55FDD6D7B6`
- 验证方式：直接在本分支的干净生产提交上验证，不创建 detached worktree。
- 状态：等待代码审阅；本提交不自动合并或推送。

工作区中用户既有的 `.gitignore` 修改未进入生产提交或本证据提交。

## 工具链

- Visual Studio 2022 Developer PowerShell 17.14.35
- MSVC 19.44.35228，x64 host/target
- CMake 4.1.2
- Ninja 1.11.1
- FlowForge Toolchain：MLIR/LLVM 18.1

所有会触发编译的 CTest negative probes 均在 Visual Studio Developer PowerShell 中运行。
普通 PowerShell 缺少 MSVC/Windows SDK 环境，会使部分 probe 先因标准库头不可见而失败，
该结果不作为有效测试结论。

## 构建与测试矩阵

| 配置 | 全量 `target all` | 第二轮 | CTest |
| --- | --- | --- | --- |
| Developer / RelWithDebInfo | 通过 | `ninja: no work to do` | 93/93 |
| Developer / Debug | 通过 | `ninja: no work to do` | 80/80 |
| Hardened Object/UI/Simulation contracts | 通过 | `ninja: no work to do` | 98/98 |
| Desktop PLAYER / RelWithDebInfo | 通过 | `ninja: no work to do` | 77/77 |
| EDITOR / RelWithDebInfo | 通过 | `ninja: no work to do` | 96/96 |
| TOOLCHAIN + FlowForge/MLIR | 通过 | `ninja: no work to do` | 77/77 |

Debug 首次并行 CTest 曾因多个 negative probe 并发写 Ninja `.ninja_deps` 产生基础设施竞争；
修复该构建日志后，以串行完整 CTest 得到 80/80，并再次确认全量构建无工作。

按本轮规范及 `AGENTS.md`，Android 不再属于默认验证矩阵。本轮没有执行 Android configure、build、
CTest 或 closure validation；仅保持公共头安装镜像同步要求。

## 架构、安装与消费端

- 源码架构扫描通过，旧 `engine/world`、`engine/simulation`、`engine/toolchain`、`engine/graph_kit`
  物理 root 及退休 public include、target、namespace 和兼容 shim 均未恢复。
- fresh install 的安装树架构扫描通过。
- 14 个 installed consumers 全部完成 configure、build 和运行；第二次构建均为 no-work。
- 新增 consumer 覆盖 Node Graph Editor、FlowForge L0 model 和 FlowForge compiler。
- Desktop PLAYER target closure 不含 Node Graph Editor、FlowForge compiler、MLIR 或 LLVM。
- FlowForge compiler 可在 TOOLCHAIN profile 独立于 Editor 和 SimulationDescription 构建、测试和消费。

## Exception contract

- runtime/domain public failure 使用 `noexcept + expected/error`；生产代码不再主动 `throw`。
- `TaskExecutor` 使用 fallible `create()`，区分 allocation 与 worker/thread creation failure。
- Event payload 的 nothrow copy/move/destruction 约束有负向编译测试。
- World transaction rollback、codec malformed/limit/allocation、FlowForge foreign exception containment
  均由结构化错误测试覆盖。
- `ByteWriter::takeOrThrow()` 与 throwing `TaskExecutor` constructor 已删除，未保留兼容入口。

## 提交序列

1. `776dc0cc` — ScriptSystemDescription 代码风格基线
2. `0f625f21` — 规范、架构 SSOT 与门禁
3. `51e110c5` — FlowForge 删除 Simulation binding ownership
4. `5c96d736` — graph_kit 迁移为 Node Graph Editor
5. `a98346ab` — FlowForge L0 model 与 Toolchain compiler 分离
6. `3f180a71` — World/Simulation/Toolchain source topology 迁移
7. `eb88fa87` — Simulation systems/scripting/transform target 拆分
8. `1ac2db63` — runtime 与 toolchain exception contract 闭环
9. `8ce06922` — installed consumers、架构扫描与最终验证修正

本 evidence-only 提交的唯一父提交必须是
`8ce0692212386796090cc3df8aae6e463670adac`。
