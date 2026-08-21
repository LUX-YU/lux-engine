# Resource：Description 与 Asset 重构

> 恢复 Resource 的通用 SDK 语义，保留既有资产机制，并把 Engine/ECS 场景语义移出 Resource

**执行文档 03 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Resource、Asset、Toolchain、ECS Scene Format、Render Content 与 Engine Asset 负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** 本文原有“文件协议与 Engine AssetStore 分离”、新建 `AssetId/AssetTypeId`、把 Terrain/Tilemap/Physics3D 长期放在 Resource 的目标已由 `ADR-20260820_SceneAsset与Resource边界.md` 取代。现有 AssetManager/SerDeser/Catalog/VFS/Pak 是公共 SDK 的正式组成；Scene 是 Engine-owned Asset，三类场景 Payload 最终归对应 ECS 领域。

> **2026-08-21 裁决更新：** Asset 公共面按资产领域族组织，存储面收敛到 `asset/storage`；Pak v2 读、写和检查均属 Modules Asset SDK。`BuiltinAssetIds.hpp`、M_Missing 与演示色板迁入 `engine/content`。详见 `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md`。


## 1. 最终边界

`modules/resource` 最终只有两个一级目录：

```text
modules/resource/
├── description/
└── asset/
```

两者职责：

| 目录 | 负责 | 不负责 |
| --- | --- | --- |
| `description` | 被动值类型、不可变数据形状、与设备/场景无关的资源描述 | Asset Cache、Scene、Extension、产品 Manifest、Editor、运行线程 |
| `asset` | `asset_id_t`、`LuxAsset`、`AssetManager`、`AssetRef`、Header、SerDeser、Catalog、Provider、VFS、Loose/Pak | Engine Scene 语义、GPU 上传、ECS Scene Payload、Editor ContentIndex |

## 2. 当前文件逐项归属

| 当前内容 | 目标 | 动作 |
| --- | --- | --- |
| `modules/resource/description/AnimationClip.hpp` | `description/animation/AnimationClip.hpp` | 保留 |
| `.../Skeleton.hpp` | `description/animation/Skeleton.hpp` | 保留 |
| `.../Mesh.hpp`、`Vertex.hpp`、`Model.hpp` | `description/mesh/*` | 保留/整理 |
| `.../Texture.hpp`、`TextureAtlas.hpp` | `description/image/*` | 保留/整理 |
| `.../MaterialEnums.hpp` | `description/material/Material.hpp` | 保留纯值 |
| `.../Shader.hpp`、`ShaderInfo.hpp` | `render/shader` 与 `description/shader` 分拆 | SPLIT |
| `.../LayoutContract.hpp` | `modules/function/render/shader/LayoutContract.hpp` | MOVE |
| `.../RenderRepresentation.hpp` | `modules/function/render/Representation.hpp` | MOVE |
| `.../ImportedMaterialDesc.hpp` | `engine/toolchain/model_import/ImportedMaterial.hpp` | MOVE |
| `.../MaterialGraphContract.hpp` | `engine/authoring/material` 或 `engine/toolchain/material` | MOVE |
| `.../Script.hpp` | `modules/function/script` 的脚本值协议 | MOVE/SPLIT |
| `modules/resource/spatial` | `modules/core/math` | MOVE/DELETE |
| `modules/resource/deployment` | `render/config` + `engine/game/deployment` | SPLIT/DELETE |
| `modules/resource/entity_scene` | `ecs/scene_format` + `engine/scene/package` | SPLIT/DELETE |
| `modules/resource/spatial3d_scene` | `engine/spatial3d` | MOVE/DELETE |
| `modules/resource/classic_mesh` | `render/standard/content` + `asset/codecs` | SPLIT/DELETE |
| `modules/resource/physics3d` | 历史先归 Resource；现行最终 owner 为 `ecs/physics3d` | SECONDARY MOVE/DELETE |
| `modules/resource/terrain` | 历史先归 Resource；现行最终 owner 为 `ecs/terrain` | SECONDARY MOVE/DELETE |
| `modules/resource/tilemap` | 历史先归 Resource；现行最终 owner 为 `ecs/tilemap` | SECONDARY MOVE/DELETE |

## 3. `description` 的严格准入合同

### 3.1 允许内容

```text
普通 struct / enum / strong ID
数组、字符串、向量等值字段
纯校验函数
与格式自身相关的 schema version
不持有线程、设备、文件、registry 的数据
```

### 3.2 禁止内容

```text
AssetManager / AssetRef 生命周期
Renderer Handle
World / Entity / Component
Scene Feature
Extension Requirement
Game Launch Policy
Editor Import State
Toolchain 中间流程状态
Runtime Capacity Planner
全局 Registry
```

### 3.3 目录只做源码组织，不自动变成组件

目标：

```text
description/
├── include/lux/description/
│   ├── Image.hpp
│   ├── Mesh.hpp
│   ├── Material.hpp
│   ├── Shader.hpp
│   ├── Animation.hpp
│   ├── Skeleton.hpp
│   ├── Physics.hpp
│   ├── Terrain.hpp
│   └── Tilemap.hpp
├── src/
│   ├── image/
│   ├── shader/
│   └── validation/
└── CMakeLists.txt
```

默认只有一个公共目标：

```cmake
lux::description
```

若编译时间需要拆分，使用私有 Object Library：

```cmake
add_library(description_shader_obj OBJECT ...)
target_sources(lux_description PRIVATE
    $<TARGET_OBJECTS:description_shader_obj>)
```

外部用户不应为了使用 `TerrainTile` 再理解 `terrain_content` 安装组件。

## 4. Description 文件处理

### 4.1 保留的纯值类型

保留并清理：

```text
AnimationClip
Skeleton
Mesh
Model
Vertex
Texture
TextureAtlas
Material enums
Tilemap pure values
```

清理要求：

- 移除 `lux/engine/...` Include Prefix；
- 移除 Runtime Reflection 注解依赖；
- 将反射元数据生成清单放到消费者 sidecar；
- 不包含 Asset ID，除非资源描述本质需要跨 Asset 引用；优先使用逻辑索引或显式 `AssetId`；
- 不包含 Renderer 内部句柄；
- 不包含 Editor 专用字段。

### 4.2 `LayoutContract.hpp`

MOVE：

```text
modules/resource/description/include/lux/engine/description/LayoutContract.hpp
→ modules/function/render/shader/include/lux/render/shader/LayoutContract.hpp
```

原因：它定义 Descriptor Set、Binding、Update Frequency、Engine-shared Set 和 Render Feature Resource，是 Renderer 与 Shader Toolchain 的协议。

相关消费者改为共同依赖：

```text
lux::render_shader_contract
```

或者将其作为 `lux::render` 的小型 header-only 子目标。不得让 Asset、Animation、Navigation 因此依赖 Render。

### 4.3 `RenderRepresentation.hpp`

MOVE 到 Render：

```text
lux::rdesc::RenderRepresentationId
→ lux::render::RepresentationId
```

将当前对 `extension_abi/StableId.hpp` 的 include 改为直接使用 `lux-cxx::StableNameId`。

标准 ID：

```cpp
inline constexpr auto classicMeshRepresentation =
    representationId("lux.render.geometry.classic_mesh");
```

属于 Render API，不属于 Extension ABI。

### 4.4 `ImportedMaterialDesc.hpp`

MOVE 到：

```text
engine/toolchain/model_import/include/lux/engine/toolchain/model_import/ImportedMaterial.hpp
```

它是 Assimp/Model Importer 与 Material Graph Converter 之间的中间结构；不应为了两个 Toolchain 消费者而下沉到 Resource。

若未来外部模型导入库确有使用需求，可再抽为独立 `lux-model-import` 公共库；不能提前污染 `description`。

### 4.5 `MaterialGraphContract.hpp`

按实际内容拆分：

```text
纯运行材质参数布局 → render/material
Authoring 节点图契约 → engine/authoring/material
Cooked 编译协议 → engine/toolchain/material
```

不得保留一个同时覆盖 Authoring Graph、Runtime Shader 与 Asset 的万能 Contract。

### 4.6 `Script.hpp`

纯 Script Signature/Value 迁往 `modules/function/script`。Asset 中只保存 Script Payload 与 Type ID；Description 不负责 Script Runtime Module。

## 5. Asset 公共层重新设计

### 5.1 删除闭合 `EAssetType`

当前 `EAssetType` 中央枚举阻止外部库添加新 Asset 类型，并混合 Authoring 与 Runtime 类型。替换为开放 ID：

```cpp
namespace lux::asset
{
    struct AssetTypeIdTag final {};
    using AssetTypeId =
        lux::cxx::StableNameId<AssetTypeIdTag>;
    using AssetTypeIdView =
        lux::cxx::StableNameIdView<AssetTypeIdTag>;

    [[nodiscard]] constexpr AssetTypeIdView
    assetType(std::string_view name) noexcept
    {
        return AssetTypeIdView{name};
    }
}
```

各领域定义自己的 ID：

```cpp
namespace lux::description::types
{
    inline constexpr auto texture =
        asset::assetType("lux.asset.texture");
    inline constexpr auto mesh =
        asset::assetType("lux.asset.mesh");
}
```

Authoring 类型放在 Authoring：

```cpp
inline constexpr auto materialGraph =
    asset::assetType("lux.authoring.material_graph");
```

### 5.2 `asset_id_t` 迁移

CREATE：

```cpp
namespace lux::asset
{
    using AssetId = uuids::uuid;
    inline constexpr AssetId nullAssetId{};
}
```

兼容：

```cpp
using asset_id_t [[deprecated("use AssetId")]] = AssetId;
```

新代码禁止 `_t` 风格。

### 5.3 删除 `LuxAsset : LuxObject`

当前 `LuxAsset`：

- 继承 Runtime Reflection `LuxObject`；
- 以虚函数暴露 `void* rawData()`；
- 依赖闭合 `EAssetType`；
- 同时携带元数据、可变 payload、CPU unload 状态。

目标公共 Asset 层不需要对象继承树。建立文件与解码结果模型：

```cpp
namespace lux::asset
{
    struct AssetHeader final
    {
        AssetId id;
        AssetTypeId type;
        std::uint32_t schemaVersion{};
        std::uint64_t payloadBytes{};
    };

    class DecodedAsset final
    {
    public:
        DecodedAsset() = default;

        template<class T>
        static DecodedAsset make(
            AssetId id,
            AssetTypeId type,
            std::unique_ptr<T> value);

        [[nodiscard]] AssetId id() const noexcept;
        [[nodiscard]] AssetTypeIdView type() const noexcept;

        template<class T>
        [[nodiscard]] const T* get(AssetTypeIdView expected) const noexcept;

    private:
        AssetId id_{};
        AssetTypeId type_;
        std::unique_ptr<void, void(*)(void*)> value_;
    };
}
```

`DecodedAsset` 只表达一次解码结果；缓存、共享观察与驱逐由 Engine `AssetStore` 决定。

### 5.4 Codec API

```cpp
namespace lux::asset
{
    struct DecodeRequest final
    {
        AssetHeader header;
        std::span<const std::byte> payload;
    };

    class Codec
    {
    public:
        virtual ~Codec() = default;
        virtual AssetTypeIdView type() const noexcept = 0;

        virtual expected<DecodedAsset, DecodeError>
        decode(const DecodeRequest&) const = 0;

        virtual expected<std::vector<std::byte>, EncodeError>
        encode(const DecodedAsset&) const = 0;
    };

    class CodecRegistry final
    {
    public:
        expected<void, CodecRegistryError>
        add(std::unique_ptr<Codec> codec);

        const Codec* find(AssetTypeIdView type) const noexcept;
    };
}
```

若 Codec 需要模板化以避免 type erasure，可提供 `TypedCodec<T>` adapter，但注册表对外仍保持开放 Type ID。

### 5.5 Provider 与 VFS

Provider 负责 `asset_id_t`/`VirtualPath` 到 opaque bytes 的字节来源，不负责解析、对象缓存、驻留或引用计数。现有 `IAssetProvider` 与 `AssetVfs` API 保持，但公共头迁入 `asset/storage`。

`AssetManager + AssetRef` 是唯一引用账本。Scene Section 等非驻留记录可以复用 Provider/VFS 读取字节，但不注册到 AssetManager，不创建 AssetRef。

### 5.6 Pak

保留 LUXPAK v2 wire。`PakAssetProvider`、writer 和 inspector 统一迁入 `asset/storage/pak`，并作为公共 Asset SDK API。公开存储面只表达：

```text
AssetId
AssetHeader
Provider
Reader
Hash
```

不得依赖 Engine 产品语义，Writer 也不使用 `EAssetType`/Catalog 解释 magic。Toolchain 保留 source 扫描、auxiliary 剔除、Catalog 策略、冲突诊断和发布组合，不得 include Asset `pinclude`。

## 6. `AssetManager` 上移

MOVE：

```text
modules/resource/asset/src/core/AssetManager.cpp
modules/resource/asset/include/.../AssetManager.hpp
→ engine/assets/src/AssetStore.cpp
→ engine/assets/include/lux/engine/assets/AssetStore.hpp
```

目标职责：

```text
已解码对象缓存
Asset Handle / generation
引用与驱逐
异步 load 协调
内存预算
主线程状态提交
Asset loaded/removed facts
```

目标接口：

```cpp
namespace lux::engine::assets
{
    class AssetStore final
    {
    public:
        LoadTicket load(asset::AssetId id);
        AssetHandle find(asset::AssetId id) const noexcept;
        void release(AssetHandle);
        void setBudget(MemoryBudget);
        CloseTask close();

    private:
        asset::Vfs& vfs_;
        asset::CodecRegistry& codecs_;
        execution::Executor& executor_;
    };
}
```

注意：这是 Engine 类型，因此 Include Prefix 保留 `lux/engine/assets` 是合理的。

## 7. 内置 Asset 领域组织

单一 `lux::engine::resource::asset` target 保持不变，但源码、头和测试按资产领域族共同组织：

```text
modules/resource/asset/
├── include/lux/engine/resource/asset/
│   ├── ...资产核心接口与 AssetCodecCatalog.hpp
│   ├── texture/ material/ mesh/ model/
│   ├── animation/ shader/ script/
│   └── storage/{AssetProvider,AssetVfs,VirtualPath,pak/...}.hpp
├── pinclude/lux/engine/resource/asset/
│   ├── detail/...
│   ├── <domain>/*DescriptionCodec.hpp
│   └── storage/pak/PakCodec.hpp
├── src/
│   ├── <domain>/...
│   └── storage/...
└── test/<domain-or-storage>/...
```

`TextureCodec/ModelCodec` 改名为 `TextureSerDeser/ModelSerDeser`。跨领域组合继续使用 `runtimeAssetCodecCatalog()`，不新建第二套 registry。`BuiltinAssetIds.hpp` 不是通用 Asset 合同，迁入 `engine/content`。

## 8. 其他 Resource 目录迁移

### 8.1 `deployment`

SPLIT：

```text
RuntimeCapacity*
    → modules/function/render/config/RenderCapacity.hpp
      仅保留真正的 GPU/Renderer capacity

RuntimeLaunchManifest*
    → engine/game/deployment/GameManifest.hpp
```

`GameManifest` 应包含：

```text
title
content packs
boot scene/package
required extensions
product options
```

不再出现 `engine_pak`；改为语义中立：

```text
base_pack
game_pack
```

Extension Entry 使用 `engine/extensions/api`，因此该 Manifest 必须位于 Engine。

### 8.2 `entity_scene`

SPLIT 详见文档 05：

```text
EntitySection wire image / persistent IDs / component schema
    → ecs/scene_format

RequiredExtension / SceneContribution / startup package
    → engine/scene/package
```

### 8.3 `spatial3d_scene`

MOVE：

```text
Spatial3DSceneCatalog
Spatial3DResidencyCapacity
lux.spatial3d.* demand channels
→ engine/spatial3d/world_partition
```

它是 Lux Engine World Partition Feature 配置，不是公共 Resource。

### 8.4 `classic_mesh`

拆为：

```text
通用 Mesh/Instance 值
    → description/mesh

标准 Renderer 的 batch content
    → render/standard/content

对应 Asset Codec
    → asset/codecs/render_standard
```

`kClassicMeshBatchContentTypeName` 属于标准 Renderer，不应成为 Resource 根组件。

### 8.5 `terrain`、`tilemap`、`physics3d`

数据值与 Codec 分离：

```text
TerrainTileBlobV1            → description/terrain
TerrainTileCodec             → asset/codecs/terrain

TilemapChunkBlobV1           → description/tilemap
TilemapChunkCodec            → asset/codecs/tilemap

StaticColliderBatch3D        → description/physics
StaticColliderBatch3DCodec   → asset/codecs/physics
```

它们不再出现在 `install_components(lux-engine-resource ...)` 的一级列表中。

## 9. CMake 目标

目标 `modules/resource/CMakeLists.txt`：

```cmake
add_subdirectory(description)
add_subdirectory(asset)

install_components(
    PROJECT_NAME lux-resource
    VERSION      1.0.0
    NAMESPACE    lux
    COMPONENTS
        description
        asset
)
```

`description/CMakeLists.txt` 当前仍保持既有 `extension_abi` 闭包；本轮只归一化 Asset
目录和组件，不推进 Extension ABI/M0 的下沉或删除。

`asset/CMakeLists.txt` 仅公开声明现有有效闭包（Core Math/Meta/Serialization、
Description、stduuid 与 lux-cxx 的 compile_time/memory），算法、核心工具和反射运行时
保持 PRIVATE；不链接 ECS 或 Engine Runtime。

## 10. 文件格式兼容迁移

### 10.1 Asset Header

先保持 wire layout，建立新外观：

```cpp
struct LegacyAssetInfoV2;
struct AssetHeaderV3;
```

读取路径：

```text
probe magic/version
→ decode legacy header
→ convert to AssetHeader
→ select open AssetTypeId
```

写入路径在单独格式升级 PR 切换。

### 10.2 `EAssetType` 转换表

建立只读 legacy adapter：

```cpp
expected<AssetTypeId, LegacyTypeError>
fromLegacyAssetType(EAssetType);

expected<EAssetType, LegacyTypeError>
toLegacyAssetType(AssetTypeIdView);
```

仅旧格式 Codec 使用；业务代码不得继续 switch `EAssetType`。

### 10.3 Golden Tests

为每种现有资产保存至少一个固定二进制样本：

```text
texture
mesh
model
material
material instance
shader
script
skeleton
animation clip
texture atlas
flow graph
entity scene / section
```

新 Reader 必须读取全部旧样本。

## 11. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| RES-01 | 建立新 `lux::description`、`lux::asset` Alias 和新 Include | 无 wire 变化 |
| RES-02 | Stable `AssetId/AssetTypeId` 与 legacy adapter | 旧资产全可读 |
| RES-03 | CodecRegistry 开放化，去中央 switch | 标准 Codec 测试通过 |
| RES-04 | `LuxAsset` 消费者迁到 DecodedAsset/领域值 | 无 `rawData()` 新调用 |
| RES-05 | `AssetManager → engine/assets/AssetStore` | Modules SDK 无缓存语义 |
| RES-06 | `spatial → math`、Description 文件归位 | Resource 依赖闭包收窄 |
| RES-07 | Terrain/Tilemap/Physics/ClassicMesh 合并到 Description+Codecs | 一级组件删除 |
| RES-08 | Deployment/EntityScene/Spatial3DScene 上移 | Resource 无 Engine 协议 |
| RES-FINAL | 删除旧 targets、旧 include、`EAssetType` 业务使用 | Resource 只剩两个一级语义 |

## 12. 验收闸门

- [ ] `modules/resource` 根只有 `description` 与 `asset` 两个一级目录。
- [ ] `lux::description` 不依赖 Extension ABI、ECS、Engine、Editor。
- [ ] `lux::asset` 不定义 `AssetManager`。
- [ ] `lux::asset` 不继承 `LuxObject`。
- [ ] 新 Asset 类型无需修改中央 enum。
- [ ] `RuntimeLaunchManifest` 不在 Resource。
- [ ] `EntitySceneManifest` 的 Engine Feature 部分不在 Resource。
- [ ] `LayoutContract` 不在 Description。
- [ ] `ImportedMaterialDesc` 不在 Description。
- [ ] `resource/spatial`、`classic_mesh`、`terrain`、`tilemap`、`physics3d` 一级 targets 已删除。
- [ ] 旧 `.luxasset` Golden Files 全部可读。
- [ ] Asset-only 外部样例不链接 ECS/Engine/Reflection Registry。
