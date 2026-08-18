# ECS Identity 与 Scene Format 边界

## 状态

- 决议日期：2026-08-18
- 实施阶段：ECS identity / LXES format foundation
- 适用目标：`lux::engine::ecs::identity`、`lux::engine::ecs::scene_format`

## 决议

`modules/` 是可独立分发的公共 SDK 边界，不能因为 Toolchain、Runtime 与
ECS 同时使用某种磁盘结构，就把 Entity、Component 或 Scene Feature 语义下沉到
`modules/resource`。

本阶段建立两个 ECS 所有者：

1. `ecs/identity`：拥有 ECS 稳定身份。首批包含 `ComponentSchemaId`，并预留与
   旧场景格式显式转换所需的 `PersistentEntityId`。
2. `ecs/scene_format`：拥有纯 LXES v1 EntitySection 数据结构、确定性有界编解码、
   内容摘要与 persistence journal。

`ecs/scene_format` 禁止依赖或出现：

- Engine Extension / `ExtensionId`；
- Scene Feature / `SceneFeatureId`；
- AssetStore、Renderer、Editor 或产品启动策略；
- EnTT Registry、运行中 Entity 句柄或具体 System 实例。

## 兼容策略

当前 `modules/resource/entity_scene` 仍是旧 LXSC/LXES 入口。它不能反向依赖 ECS，
因此本阶段不把旧头文件改成指向 ECS 的 forwarding header，也不在 `modules/`
增加任何 ECS 依赖。

迁移期间由 Engine 层的 `entity_section_wire_compatibility_test` 同时链接新旧实现，
验证以下协议保持字节一致：

- 最小 LXES v1 EntitySection image；
- EntitySection content digest；
- Persistence journal record envelope。

旧路径仅作为临时兼容实现。任何新 ECS 代码必须使用 `ecs/identity` 或
`ecs/scene_format`，不得新增 `modules/resource/entity_scene` 依赖。

## 本阶段完成范围

- `ComponentSchemaId` 从 `ComponentTypeCatalog.hpp` 内联定义中抽出，由
  `ecs/identity` 统一拥有；
- `ecs/core` 不再为 Component Schema canonical-name 校验依赖 Extension ABI；
- 建立纯 `ecs/scene_format` target、安装组件和独立契约测试；
- 建立新旧 LXES / journal 字节兼容测试；
- 保留现有 Runtime、Toolchain 与 Authoring 对旧 Scene Manifest 的使用，避免在
  Scene Package 边界建立前发生大爆炸式迁移。

## 后续阶段

1. 在 `engine/scene/package` 建立 LXSC Scene Package，拥有 RequiredExtension 与
   Scene Feature selection；
2. 建立 Legacy EntityScene ↔ ScenePackage / ECS SceneFormat 显式转换；
3. 迁移 Toolchain producer 与 Runtime consumer；
4. 删除旧 LXES 实现，只保留阶段性 forwarding/adapter；
5. 全仓无消费者后删除 `modules/resource/entity_scene`。
