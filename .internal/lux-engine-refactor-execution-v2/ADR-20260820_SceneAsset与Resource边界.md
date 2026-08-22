# ADR：Scene Asset 与 Resource 边界

**状态：** Accepted

**日期：** 2026-08-20

**适用基线：** `codex/scene-asset-redesign` 及后续提交
**取代范围：** 执行文档 v2 中关于新建 `AssetId`、`AssetTypeId`、`engine/assets/AssetStore`，以及把场景领域 Payload 永久放在 Resource 的目标

## 1. 背景

`modules/` 是可以被外部项目独立使用的通用库，`engine/` 才承载 LUX Engine 的场景、ECS 装配和产品语义。现有 Resource Asset 已经提供完整且抽象的资产机制：

- `asset_id_t = uuids::uuid`；
- `LuxAsset`、`TAsset<T>` 与 `TAssetSerDeser<Config>`；
- `AssetManager`、`AssetRef`、revision、驻留票据、驱逐与广播；
- `AssetCodecCatalog`、Provider、VFS、Loose/Pak；
- `engine/runtime/assets` 对现有异步执行管线的加载编排。

因此，把 `AssetManager` 复制或上移为第二套 `AssetStore`，以及仅为改名引入 `AssetId`、`AssetTypeId`，都会制造重复概念而不能改善依赖方向。

另一方面，Scene、Entity Section、Terrain、Tilemap 和 Physics3D 场景 Payload 带有明确的 Engine/ECS 语义。它们不应由通用 Resource 解释，但 Scene 本身仍应复用公共资产机制获得 VFS、Pak、异步加载和驻留能力。

## 2. 决议

### 2.1 公共 Resource Asset 保持完整

`modules/resource/asset` 继续拥有通用的：

```text
asset_id_t
LuxAsset / TAsset<T>
AssetManager / AssetRef
TAssetSerDeser
AssetCodecCatalog
Provider / VFS / Loose / Pak
```

不创建：

```text
AssetId
AssetTypeId
engine/assets
AssetStore
第二套资产 Registry 或全局可变 Codec Registry
```

`EAssetType` 保留为现有资产头的稳定数值类型。Resource 只定义通用资产的枚举成员；Engine-owned 类型可以保留兼容数值，但不能把 Engine 名称和语义写回 Resource。

### 2.2 Engine Scene 是公共资产机制上的领域资产

`engine/scene` 是一个组件，只拥有：

```text
SceneDescription
SceneAsset : lux::asset::TAsset<SceneDescription>
SceneAssetSerDeser : lux::asset::TAssetSerDeser<std::monostate>
Scene Feature 领域 ID
LXSC v1 的同步、bounded、无副作用验证与 Codec
```

它不拥有：

```text
IO 线程或异步执行器
第二套 AssetManager
entt::registry 生命周期
SceneRuntime 生命周期
Editor 或产品启动策略
```

`SceneAssetSerDeser` 遵循现有 Mesh/Texture SerDeser 形状，提供 `encodeData()`、`decodeData()`，并实现 `fromLuxAssetStream()`、`exportAsLuxAssetStream()`。不得在 `SceneAsset` 上添加静态 encode/decode。

### 2.3 Scene wire 合同

- 新 Scene Asset magic 为 `0x0130914D`。
- 外层使用标准 `AssetFileHeader`。
- data 区逐字节保存 LXSC v1；LXSC v1 的 magic、版本、字段顺序与数值宽度不变。
- 历史裸 LXSC magic `0x4353584C` 是同一 Scene descriptor 的只读 legacy magic。
- 裸 LXSC 可以解码并由内部 ID 合成 `AssetInfo`；再次导出只写新包裹格式。
- 外层 `AssetInfo::id` 与内层 Scene ID 必须非 nil 且相等。
- LXES v1 Section 是 Scene-owned Pak 内部记录，不注册为可驻留 Asset，不改变现有 content path。

### 2.4 Catalog 是唯一类型分派点

产品从标准 Resource descriptors 复制一份不可变 descriptor 列表，追加 `sceneAssetCodecDescriptor()` 后调用现有 `AssetCodecCatalog::build()`。

Catalog 负责：

- 按 type 查找；
- 按主 magic 或 legacy magic 查找；
- 拒绝重复 type、重复 magic、C++ type hash/name 冲突；
- 必要时通过 descriptor 的 image-to-shell 回调为无标准 Header 的 legacy image 创建 shell。

`AssetManager`、异步加载、Pak 验证和 Toolchain 分派统一使用 Catalog。Resource 不再维护 `assetTypeOfMagic()` 中央 switch，也不解释 Scene magic。

### 2.5 Provider 与 Pak 保持语义不透明

- `ProviderEntry` 暴露原始 `magic_number`，不把 Engine Scene 解释为 Resource enum 名称。
- `PakCookFileEntry` 直接接收 magic。
- Pak v2 的 `asset_magic` 字段及其字节格式不变。
- `RuntimeLaunchManifest` 必须显式给出 boot Scene 路径；Resource 不再提供“唯一 Scene 自动选择”的规则。

### 2.6 场景 Payload 由 ECS 领域拥有

以下类型、Codec 与契约测试从 Resource 二次归位到已有 ECS 领域 target，类型名、namespace 和 wire 不变：

| Payload | 最终 owner | Wire |
| --- | --- | --- |
| `TerrainTileBlobV1` / Codec | `ecs/terrain` | LXTT v1 |
| `TilemapChunkBlobV1` / Codec | `ecs/tilemap` | LXTC v1 |
| `StaticColliderBatch3DBlobV1` / Codec | `ecs/physics3d` | LXPC v1 |

这些 Payload 只有在对应 ECS 场景领域中才有意义，不属于通用 Description/Asset 类型目录。历史 `DESC-007/008/009` 表示当时 Resource 内容组件清零确已完成；本 ADR 不改写历史，而以 `SCENEASSET-*` 跟踪二次归位。

### 2.7 Runtime 与产品接入

- `engine/runtime/assets` 保留为现有 `AssetManager` 的异步编排适配器。
- Game、Editor 与相关 Toolchain 使用“标准 descriptors + Scene descriptor”的 immutable Catalog。
- Game 通过 VFS 和现有 `AssetLoadService` 加载 `SceneAsset`。
- `SceneRuntime` 消费 typed `SceneAsset` 并持有 `AssetRef`，不接收裸 LXSC image。
- Editor 临时场景也注册为 `SceneAsset`，但本 ADR 不重构 Editor 架构。
- GameExporter 输出包裹 SceneAsset，Section 继续输出裸 LXES。

## 3. 目录与 target

目标 Scene 目录：

```text
engine/scene/
├── CMakeLists.txt
├── include/lux/engine/scene/
│   ├── SceneDescription.hpp
│   ├── SceneAsset.hpp
│   ├── SceneAssetSerDeser.hpp
│   └── SceneFeatureId.hpp
├── src/
└── test/
```

单一 target/component 为 `lux::engine::scene::scene` / `scene`。旧 `scene_api`、`scene_package`、`ScenePackage`、`ScenePackageId`、旧 include 和 forwarding header 全部删除，不保留 alias。

## 4. 被取代的旧目标

以下 Checklist 项不再是待办，也不记为完成，统一标记“被 ADR-20260820 取代”：

- `ASSETSDK-001/002/005/016/017/018`
- `ENGEA-010..020`
- `SCENE-001`
- `SCENE-010`

其它使用 `AssetStore`、`AssetId`、`AssetTypeId` 或 `ScenePackage` 作为未来接口的章节，以本 ADR 为准解释；与本 ADR 冲突的示例不再具有施工约束力。

## 5. 不变边界

- 不推进 M0、全局 M7 命名迁移、Registry 重构、Editor 架构重构或 Extension ABI v4。
- 不重写 standalone Asio/stdexec/TBB 管线。
- 不改变 LXSC v1、LXES v1、LXTT v1、LXTC v1、LXPC v1 的内部字节。
- Classic Mesh、Texture、Mesh、Material 等通用内容保持当前 owner。

## 6. 验收原则

- legacy 裸 LXSC 可读，新格式 decode/re-encode 确定；新 data 区与 LXSC Golden 逐字节一致。
- Catalog 的 type/magic/C++ type 冲突和 legacy magic 路径有 owner tests。
- Resource 的生产代码、Pak 与 Provider 不再含 Scene 名称或 boot Scene 策略。
- 三类领域 Payload 在新 owner 下保持长度、SHA 与 decode/re-encode 契约。
- 安装包只暴露 canonical `scene` component；旧 Scene components 和 Resource 场景 Payload 头不存在。
