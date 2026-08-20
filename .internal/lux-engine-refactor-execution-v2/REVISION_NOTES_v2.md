# v2 相对 v1 的架构修订说明

## 2026-08-20：Scene Asset 与 Resource 边界修订

- 接受 `ADR-20260820_SceneAsset与Resource边界.md`。
- 保留公共 `asset_id_t`、`LuxAsset/TAsset`、`AssetManager`、`TAssetSerDeser`、Catalog、Provider、VFS 与 Pak。
- 取消新建 `AssetId`、`AssetTypeId`、`engine/assets/AssetStore` 和第二套资产系统的目标。
- 将 Scene 定义为既有资产机制上的 Engine-owned Asset；`engine/scene` 只负责同步数据、验证和 SerDeser。
- 将 Terrain/Tilemap/Physics3D 场景 Payload 的最终 owner 改为对应 ECS 领域，保留历史 Resource 内容组件清零记录。
- 用 `SCENEASSET-*` 条目追踪二次归位，不把被取代的旧目标伪装为已完成。

## 2026-08-20 Scene Asset 实施完成

- `SCENEASSET-001..020` 已完成并通过 owner contract、四 Profile 全量构建、安装 consumer 与旧符号归零验收。
- `engine/scene/api` 与 `engine/scene/package` 已删除，canonical owner 为单一 `engine/scene` component。
- Terrain、Tilemap、StaticColliderBatch3D 值/Codec/tests 已二次归位对应 ECS 领域，五种既有内部 wire version 均未升级。
- CTest 当前未注册测试；验收结果明确来自独立 owner test executables，不将 0 项 CTest 误报为覆盖。

> 明确废止的旧建议、保留的上层裁决以及新版施工顺序变化

**执行文档 R · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-20 |
| 适用对象 | 此前文档使用者、项目负责人、代码评审者 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。


## 废止

- 废止 `engine/module/include/lux/module/Modules.hpp`。
- 废止把 Extension 全面改名为 Module。
- 废止把 Scene/Render/ECS/Operation 统一注册到一个公共 Module 容器。
- 废止让公共 Resource 承载 Deployment、EntityScene、WorldPartition。
- 废止把 `AssetManager` 原样改名后继续作为公共 Resource owner。
- 废止让 ECS Core 依赖 Engine Extension ABI 与 Engine Asset 管理。
- 废止先重构 Game/Editor、后处理公共模块边界的施工顺序。

## 保留

- 不创建公开 EngineRuntime。
- Game 与 Editor 是独立产品。
- Editor Play 复用 game::Session。
- Schedule 唯一拥有 System。
- Panel 构造注入精确依赖。
- 命令与事实分离。
- 显式关闭协议。
- Workspace/Workbench/Documents 拆分。
- Flow/Material/Script 从 Panel 中拆出 Compiler/Document/Commands。

## 新增

- `modules/` 作为独立 SDK 分发边界的准入规则。
- Resource 只保留 Description/Asset 的硬闸门。
- 格式归语义所有者的三问法。
- Build-time Generator 依赖不污染 Runtime 的规则。
- Extension ABI 单独 Engine SDK。
- 488 项施工 Checklist。
- 当前代码事实索引与 123 条迁移映射。
