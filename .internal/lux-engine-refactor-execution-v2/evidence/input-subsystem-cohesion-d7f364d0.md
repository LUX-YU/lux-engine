# GAPI 保留与单体 Input 子系统施工证据

**施工基线：** `d7f364d0`
**文档裁决提交：** `e994a42a`
**实施提交：** `08e3d590`
**日期：** 2026-08-21

## Owner 与依赖边界

- `modules/platform/gapi` production diff 为空；既有 target、component、namespace 与公共头保持。
- normalized key/mouse/modifier/state/touch/event/snapshot 已归 `lux::input`；旧 Window 输入头、namespace 与 API 扫描归零。
- Window callbacks 只积累 backend-native key/mouse/scroll/text event batch；`Input::sample()` 负责规范化、held/edge、cursor 和 sample history。
- `lux::input::Input` 拥有 Snapshot、ActionMapper、Mapper 内唯一 ActionRegistry 与 ContextStack。GameApplication、Player、Editor 不再维护第二份 Registry。
- Input 只有 `lux::engine::function::input` 一个 target/component；CMake 只选择 GLFW 或 Android 私有源。无 Adapter、backend interface、factory、catalog 或平台子 target。
- installed Input target 的 `INTERFACE_LINK_LIBRARIES` 为 `lux::cxx::container`，不传播 Window/GLFW。

## 契约测试

- `input_system_test`：26 项通过，覆盖 Action/Context priority、consume、disabled、trigger、modifier、injection、UI routing、唯一 Registry 与 synthetic evaluation。
- `input_platform_translation_test`：覆盖 unknown bounds、raw event order、同帧 press/repeat/release、最终 held、mouse、scroll、text 与 modifier。
- enum compile-time contract 固定 key code、mouse ordinal、modifier bit、input state 与 touch phase ordinal。
- `input_public_link_test` 通过；input-only installed consumer 可配置、链接、运行 synthetic Snapshot + Action Mapping。
- Window+Input installed consumer 可独立配置和链接；运行期由完整安装闭包提供 Window/GLFW DLL。

## 构建、安装与平台证据

| 验证 | 结果 |
| --- | --- |
| DEVELOPER RelWithDebInfo `target all -j 4 -k 0` | PASS；第二轮 `ninja: no work to do` |
| PLAYER RelWithDebInfo | PASS；第二轮 no-op |
| EDITOR RelWithDebInfo | PASS；第二轮 no-op |
| TOOLCHAIN RelWithDebInfo | PASS；第二轮 no-op；该离线 Profile 不配置 Input |
| CTest 四 Profile | 命令 PASS，但工程当前注册 0 项；覆盖来自 owner executables |
| Android Input selected sources | NDK Clang 19、arm64/android-28 `-fsyntax-only` PASS |
| Android PLAYER 完整配置 | 正确 Android lux-cxx/Host Tools 后进入 Render codegen，但配置阶段无输出停滞，人工终止；未伪报全量通过 |
| Debug/RelWithDebInfo/Android headers | 新 Input/Window 头同步；旧 Window `InputSnapshot.hpp`、`LuxWindowDefination.hpp` 精确删除 |
| installed consumer | input-only 与 Window+Input configure/build/run PASS |
| 旧符号与结构扫描 | 旧 include/API/namespace、Adapter/interface、`input_glfw`/`input_android` 归零 |

本阶段未接入真实 Android GameActivity 输入；Android 平台实现按 ADR 确定性地产生空快照。
