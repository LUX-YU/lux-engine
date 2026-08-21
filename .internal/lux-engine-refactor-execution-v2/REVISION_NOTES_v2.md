# v2 相对 v1 的架构修订说明

## 2026-08-21：Asset 领域内聚、Pak 公共边界与 Engine Content

- 接受 `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md`。
- Asset 从 `core/codecs/pak` 横向分区改为领域族组织，Storage 作为语义领域容纳 Provider/VFS/Pak。
- Pak v2 reader/writer/inspector 是公共 Modules Asset SDK；Toolchain 只拥有 cook/publish 策略。
- `BuiltinAssetIds.hpp`、M_Missing 与内置色板归 `engine/content`；ECS fallback 改为 Engine Runtime 注入。
- 保持 AssetFileHeader v1/v2、所有资产 wire、冻结 UUID/颜色和 LUXPAK v2 字节不变。

## 2026-08-21：Asset 领域内聚实施完成

- `1364810c` 完成 Asset 领域布局、Storage/Pak 公共 API、Engine Content owner 与 fallback 注入；`e7348155` 完成 wire/storage 契约。
- 11 类标准资产与 1000-entry LUXPAK v2 fixture 固定 length/SHA；decode/re-encode、v1 读取、corrupt/truncated 与引用账本隔离测试通过。
- Toolchain 不再包含 Asset `pinclude`；Resource/ECS 不再拥有或反向依赖 Engine Builtin 身份。
- DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 的 Windows RelWithDebInfo 全量构建与第二轮 no-op 通过；installed Asset/Content consumers 通过。

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

## 2026-08-21 Asset Pipeline 与 Core Meta 裁决

- `AssetSerDeser` 成为唯一具体 Codec 多态接口；Catalog 解码完整对象，LoadService 编排，Manager 安装并维护账本。
- 删除 `AssetDataInjector`、`AssetLoadFn`、`ThumbnailLoadFn` 等退化回调；AssetRef 明确不触发 IO。
- Animation/Script/Thumbnail 的运行期需求归 Engine Runtime/Editor service 装配，ECS 不执行同步 IO。
- Registry、allocator 与 handles 归 ECS Core；Core Meta 删除 EnTT、LuxObject、EntityObject 与虚构反射根类。
- `MODULES_SDK` Profile 提案废止，改用现有四个 Profile 的 installed consumers 验证公共包闭包。
