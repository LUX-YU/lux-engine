# L1 Simulation 最终重构验证证据（2026-08-27）

## 冻结候选

- 实施基线：`673b3d17b0324b9c0a57a9316dab0a461f5bfd91`
- 被验证的生产提交：`8afeb80496a6ccd69494e9848714884b84f525cd`
- 验证方式：从被验证提交创建干净 detached worktree；验证结束后 `git status --short` 为空。
- 状态：`READY_FOR_INDEPENDENT_API_ACCEPTANCE`。本证据不代替独立 API acceptance；在该验收通过前不得标记为 `FROZEN`。

## 工具链

- CMake 4.1.2
- Ninja 1.11.1
- Windows：MSVC 19.44.35228，Visual Studio Developer PowerShell，x64 host/target
- Android：NDK 30.0.15729638、Clang 21.0.0、Vulkan headers 1.4.335、`arm64-v8a`、`android-33`、vcpkg `arm64-android`、`LUX_BUILD_PROFILE=PLAYER`

Android 必须使用 NDK 30。使用 NDK 28.2.13676358 的全新构建会在既有 Vulkan 模块因缺少
`VK_KHR_dynamic_rendering_local_read` 定义而失败；旧增量 Android 树未重新编译该模块，曾掩盖此工具链约束。

## 精确提交验证矩阵

| 配置 | 全量 `target all` | 第二轮 | CTest | 安装 |
| --- | --- | --- | --- | --- |
| RelWithDebInfo / Developer | 通过（412 steps） | `ninja: no work to do` | 82/82 | fresh install 通过 |
| Debug / Developer | 通过（416 steps） | `ninja: no work to do` | 87/87 | 不适用 |
| RelWithDebInfo / Hardened | 通过（416 steps） | `ninja: no work to do` | 87/87 | 不适用 |
| Android arm64-v8a / PLAYER | 通过（383 steps） | `ninja: no work to do` | 不在 Windows host 执行 | fresh install 通过 |

Hardened 配置显式启用 `LUX_OBJECT_CONTRACT_CHECKS=ON` 和 `LUX_UI_CONTRACT_CHECKS=ON`。
当前工程没有 `LUX_ECS_CONTRACT_CHECKS` CMake 选项；传入该变量会被 CMake 报告为未使用，
ECS 契约由源码架构扫描、negative probes 和 ECS 测试覆盖。

所有会触发编译的 CTest negative probes 均在同一 Visual Studio Developer PowerShell 中运行。
普通 PowerShell 缺少 MSVC/Windows SDK 环境，会使其中 11 项先因标准库头不可见而失败，不能作为有效测试结果。

## 安装消费端与架构门禁

- exact-SHA fresh Windows 安装前缀通过 installed architecture scan。
- exact-SHA fresh Android 安装前缀通过 installed architecture scan。
- 16 个 installed consumers 均完成 configure、build 和运行。
- `simulation-core` consumer 已删除；已删除的 L1 头、target 和安装包未保留 shim 或 alias。
- 三个 canonical 安装前缀中的本轮 L1 旧头和旧包已清除，并同步了新公共头。
- 共享 canonical 前缀仍含本轮范围外的既有项，因此最终 installed-surface 结论只取自干净 exact-SHA 安装前缀：
  RelWithDebInfo 的空 `sinclude/lux/robotics`、Debug 的 `lux/cxx/archtype/Context.hpp`，以及 Android 的
  `authoring/world/WorldSource.hpp` 不属于本轮 L1 产物。

## 性能门禁

数据来自被验证提交的 RelWithDebInfo 构建，benchmark schema version 为 5；每个规模采样三次。

| 组 | 规模 | 耗时范围 | 分配 | 通知 / callback | 反射 / 字符串查找 |
| --- | ---: | ---: | ---: | ---: | ---: |
| command-buffer | 100,000 | 1,120,800–1,148,500 ns | 0 | 不适用 | 0 / 0 |
| command-buffer | 1,000,000 | 11,074,800–12,941,400 ns | 0 | 不适用 | 0 / 0 |
| reactive-dirty | 100,000 | 460,600–469,400 ns | 0 | 100,000 / 0 | 0 / 0 |
| reactive-dirty | 1,000,000 | 4,486,200–4,501,800 ns | 0 | 1,000,000 / 0 | 0 / 0 |
| typed-event | 100,000 | 446,600–453,200 ns | 0 | 100,000 / 100,000 | 0 / 0 |
| typed-event | 1,000,000 | 4,889,900–4,928,200 ns | 0 | 1,000,000 / 1,000,000 | 0 / 0 |

prepare 后的 command-buffer 记录、reactive dirty 更新和 typed event 热路径均满足零分配；
通知与 callback 数量精确，事件热路径没有反射或字符串查找。

## 提交序列

1. `14e3ea88aa48c49762bb43134f259431ee359157` — meta noexcept、规范与门禁
2. `9011cf2b8ad16556dc773a81c810cafc1f8d68a4` — direct Registry 与 command buffer
3. `3f90135526ddf9504246faa1f6aadaffc155f735` — System metadata 与完整 Description
4. `17199041863ba414422e8cafa12b8021c8081dfa` — reactive hierarchy 与 transforms
5. `8afeb80496a6ccd69494e9848714884b84f525cd` — LXSD v2、installed consumers 与性能门禁

本证据提交的父提交必须等于上述被验证生产提交。实施前已有的用户工作树修改未进入以上提交。
