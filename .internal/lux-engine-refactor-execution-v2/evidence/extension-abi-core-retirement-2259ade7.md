# Extension ABI v4 Owner / Core 清零施工证据

**施工基线：** `2259ade7`
**文档裁决提交：** `2a916295`
**实施提交：** `c56efbc4`
**日期：** 2026-08-21

## Owner 与依赖边界

- `engine/extensions/api` 直接拥有 ExtensionId、descriptor/version/result、函数签名与三个 v4 symbol constants；installed target 的 `INTERFACE_LINK_LIBRARIES` 只有 `lux::cxx::core;lux::cxx::abi`。
- `modules/core/extension_abi` 的目录、target、component、旧 include 和安装 export 已删除；Core available components 仅为 math/meta/serialization/log/events。
- 通用 `ContributionIdTag/View/Id` 与 `contributionId()` 已删除；production、test 和 CMake 精确词扫描归零，不建立替代通用 ID。
- Authoring Project Manifest 使用 Authoring-owned extension source DTO；Game Exporter 与 Editor ProjectController 在 authoring→cooked/runtime 边界显式转换为 Engine Extension API。

## ABI v4 契约

- `kExtensionAbiV4 == 4`，ABI fingerprint 与 `lux::cxx::AbiBuildInfo::fingerprint()` 一致。
- Windows x64 固定 `ExtensionDependencyView` 为 24 bytes/alignment 8，`ExtensionModuleDescriptorV4` 为 80 bytes/alignment 8，关键 offset 为 target=62、dependencies=64、dependency_count=72；registration result 为 1 byte/alignment 1。
- `EExtensionModuleTarget` ordinal 固定 0/1；`EExtensionRegistrationError` 固定 0..7。
- 三个导出字符串保持 `luxGetExtensionModuleV4`、`luxRegisterRuntimeContributionsV4`、`luxRegisterEditorContributionsV4`。
- 动态 fixture 验证 path/memory image loading、dependency closure、validate-before-publish、component reflection commit/rollback、duplicate rejection、lease 与 unload；Editor-only registrar transaction 通过。
- 实际 `org.lux.physics2d.runtime.dll` 由 `dumpbin /exports` 确认导出 module/runtime 两个 v4 C symbols，Game Export extension smoke 通过。

## 构建、安装与消费者

| 验证 | 结果 |
| --- | --- |
| DEVELOPER RelWithDebInfo `target all -j 4 -k 0` | PASS；公共头最终重编 144/144；第二轮 no-op |
| PLAYER RelWithDebInfo | PASS 157/157；第二轮 no-op |
| EDITOR RelWithDebInfo | PASS 220/220；第二轮 no-op |
| TOOLCHAIN RelWithDebInfo | PASS 44/44；第二轮 no-op |
| Engine Extension API installed consumer | configure/build/run PASS；六个公共头可共同消费 |
| 旧 Core component 反向查找 | 按预期失败：`extension_abi` 不在 available components |
| Debug/RelWithDebInfo/Android include 前缀 | 新六头 SHA 与 source 一致；旧 Core ABI 头不存在 |
| RelWithDebInfo package exports | 新 `lux-engine-extensions/extension_api` 存在；旧 Core export 不存在 |
| module layout / target DAG | 四 Profile 配置通过；旧 target 不在 classified/unclassified graph |

四个构建树的 CTest 命令均成功执行，但工程当前注册 0 项；因此本证据明确来自 owner executables，未把空 CTest 当成覆盖。
