# Core Serialization / ECS Component Archive 施工证据

**施工基线：** `6906ccc2`
**文档提交：** `d9d3619b`
**实施提交：** `d1ead288`
**日期：** 2026-08-21

## 所有权与安装闭包

- Core Serialization 只导出 Archive、NameTable、UUID/POD/string 与 bounded `readSpan()`；导出链接闭包为 `lux::cxx::binary;stduuid`，不含 Meta、Eigen、ECS 或 Engine。
- `lux::engine::ecs::component_archive` 独立安装，PUBLIC 依赖 Core Serialization、Core Meta、compile_time，Eigen 为 PRIVATE；不含 Resource、Runtime 或 Engine。
- Function Animation 的导出闭包为 Core Math + Resource Description，不再查找 Resource Asset。
- Debug、RelWithDebInfo、Android 三个 include 前缀均含新 ECS 头且不含旧 Core TaggedPropertyArchive 头。

## 格式与行为契约

- `EArchiveType` ordinal 保持不变；源码名 `AssetRef=48` 改为 `Uuid=48`，fixture 仍为 60 bytes，tag byte 仍为 48。
- Component Archive owner test 覆盖 preflight 无副作用、详细错误、limits、compatible future field、exact canonical schema、重复/缺失/类型漂移、截断/尾随、NaN、UUID annotation 与 nested payload 边界。
- `world_source_codec_test` 继续固定 LXWA v4 与全部子文档 length/SHA；SceneFormat/LXES/Persistence、Spatial3D L3SC、Infinite2D 与 Editor roundtrip owner tests 均通过。
- Unknown Component schema 在 Authoring、Toolchain、Runtime 三条路径明确失败；Authoring/Runtime 断言 Registry 在失败前未发布部分状态。

## 构建与消费者

| 验证 | 结果 |
| --- | --- |
| DEVELOPER RelWithDebInfo `target all -j 4 -k 0` | PASS；第二轮 `ninja: no work to do` |
| PLAYER RelWithDebInfo | PASS；owner contracts PASS；第二轮 no-op |
| EDITOR RelWithDebInfo | PASS；owner contracts PASS；第二轮 no-op |
| TOOLCHAIN RelWithDebInfo | PASS；owner contracts PASS；第二轮 no-op |
| Core Serialization installed consumer | configure/build/run PASS；无 Meta/Eigen/ECS/Engine |
| ECS Component Archive installed consumer | configure/build/run PASS；无 Resource/Runtime/Editor |
| Function Animation installed consumer | configure/build/run PASS；无 Resource Asset |
| module layout / target DAG | 四 Profile 配置通过 |

四个构建树当前均报告 `No tests were found!!!`，因此验收记录来自独立 owner test executables，未把 0 项 CTest 误报为测试覆盖。旧构建目录中的 `entity_scene_contract_test.exe` 是已删除 Resource target 的历史残留，不属于当前 CMake DAG；现行 LXES 契约由 `entity_section_wire_compatibility_test`、`entity_scene_cooker_test` 与 `runtime_entity_scene_integration_test` 验证。
