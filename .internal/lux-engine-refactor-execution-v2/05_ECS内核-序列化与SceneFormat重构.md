# ECS 内核、序列化与 Scene Format 重构

> 让 ECS 重新成为真正独立的实体组件系统层，并把 Entity Scene 格式与 Engine Scene Package 分开

**执行文档 05 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | ECS Kernel、Component Reflection、Scene Serialization、Runtime Entity Loading 与 Toolchain Cooker 负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** ECS Scene Format 继续拥有 LXES/Persistence 原始记录，不直接拥有 `entt::registry` 的文件镜像。Terrain、Tilemap 与 Physics3D 场景 Payload 的值、Codec 和 wire tests 归各自 ECS 领域；Engine `SceneDescription/SceneAsset` 负责 LXSC。详见 `ADR-20260820_SceneAsset与Resource边界.md`。

> **2026-08-21 裁决更新：** ECS 不拥有、不包含 Engine 内置资产身份。Render Residency 的 fallback material ID 由 Engine Runtime 装配时注入；nil 明确表示不请求默认材质。

> **2026-08-21 Registry/资产需求裁决：** Registry 原样归 ECS Core，Core Meta 不再链接 EnTT；`AssetLoadFn` 直接删除，不创建空泛 `ecs/assets integration`。Animation/Script 的资产请求由 Engine Runtime integration 显式使用 `AssetClient`，ECS 系统只消费 ready 数据。

> **2026-08-21 实施状态：** Registry/资产需求裁决已完成。ECS Core installed consumer 不导入 Resource Asset；Animation Resolver 与 Script request system 均由 Runtime integration 私有拥有，ECS production 不执行同步资产 IO。

> **2026-08-21 Component Archive 裁决：** Reflection-driven tagged-property archive 整体归 `ecs/serialization` 的 `component_archive` component；Core 只保留 byte Archive/NameTable。不建立 RegistryArchive，Unknown Component schema 在 Authoring/Toolchain/Runtime 均拒绝。详见 `ADR-20260821_CoreSerialization与ECSComponentArchive边界.md`。

> **2026-08-21 Component Archive 实施状态：** `d1ead288` 已建立详细 expected/limits、compatible/exact reader 与 UUID annotation 语义；LXWA/LXES/Persistence/L3SC/Infinite2D owner 契约和 installed consumer 均通过。


## 1. 当前问题

`ecs/CMakeLists.txt` 对自身的定义是正确的：知道 Entity/Component 的代码属于 ECS，Function 保持 ECS-free。但 `ecs/core` 当前 PUBLIC 依赖多个错误下沉的模块，形成形式无环、语义倒置的结构。

| 当前依赖用途 | 目标 | 动作 |
| --- | --- | --- |
| `core::meta` for EntityRegistry | `ecs/core` owns Registry directly | REMOVE dependency |
| `core::extension_abi` for ComponentSchemaId | `ecs::ComponentSchemaId` | REMOVE dependency |
| `resource::asset_core` for AssetLoadFn | `ecs/integration/assets` | REMOVE from kernel |
| `resource::entity_scene` for persistent identity | `ecs/scene_format` | SPLIT |
| `resource::spatial` for Position | `lux::math` | REPLACE |

## 2. ECS Core 最终合同

ECS Kernel 只负责：

```text
Entity / Registry
World
ComponentSchema
ComponentTypeCatalog
ISystem
Schedule / ScheduleBuilder / topology
SceneServices 的装配期机制
Command Buffer / barrier
Hierarchy / Name 等真正通用组件
```

它不负责：

```text
Asset loading
Game Scene Package
Extension loading
Render backend
Window
Editor
World Partition
Script backend implementation
```

目标依赖：

```text
EnTT
lux-cxx algorithm/compile_time/container
lux::math
可选 lux::serialization 基础
```

## 3. Entity Registry 回迁 ECS

### 3.1 MOVE

```text
modules/core/meta/include/lux/engine/meta/LuxObject.hpp
modules/core/meta/include/lux/engine/meta/RegistryMemoryResource.hpp
modules/core/meta/src/RegistryMemoryResource.cpp
→ ecs/core/include/lux/ecs/Registry.hpp
→ ecs/core/include/lux/ecs/RegistryMemory.hpp
→ ecs/core/src/Registry.cpp
→ ecs/core/src/RegistryMemory.cpp
```

### 3.2 命名

```text
lux::meta::entity_id          → lux::ecs::Entity
lux::meta::EntityRegistry     → lux::ecs::Registry
lux::meta::EntityHandle       → lux::ecs::EntityHandle
lux::meta::EntityObject       → 删除或 lux::ecs::EntityObject
LuxObject                     → 删除
```

推荐不保留 `EntityObject` OO wrapper，业务代码直接使用：

```cpp
Entity entity = registry.create();
registry.emplace<Transform>(entity, ...);
```

若外部 API 确需 RAII Entity，可建立窄 `OwnedEntity`，但不得作为所有 Asset/Reflection 对象的共同基类。

## 4. Component Schema ID 归 ECS

### 4.1 CREATE

```text
ecs/core/include/lux/ecs/ComponentSchemaId.hpp
```

```cpp
namespace lux::ecs
{
    struct ComponentSchemaIdTag final {};
    using ComponentSchemaId =
        lux::cxx::StableNameId<ComponentSchemaIdTag>;
    using ComponentSchemaIdView =
        lux::cxx::StableNameIdView<ComponentSchemaIdTag>;

    [[nodiscard]] constexpr ComponentSchemaIdView
    componentSchema(std::string_view name) noexcept
    {
        return ComponentSchemaIdView{name};
    }
}
```

### 4.2 MODIFY

所有 `ComponentSchemaDescriptor`、`ComponentTypeCatalog` 与 Scene Format 使用该类型。

DELETE ECS 对：

```text
lux::engine::core::extension_abi
lux::extensions::ContributionId
```

的依赖。

## 5. Asset 依赖移出 Kernel

当前 `ecs/core` 因 `AssetLoadFn` 等类型 PUBLIC 链接 `asset_core`。施工：

### 5.1 核心层删除

从 `ecs/core` 删除：

```text
AssetLoadFn
AssetManager pointer/reference
Asset Load callback
Asset-specific SceneServices
```

### 5.2 建立 Integration

CREATE：

```text
ecs/integration/assets/
├── CMakeLists.txt
├── include/lux/ecs/integration/assets/
│   ├── AssetResolver.hpp
│   └── AssetComponentLoader.hpp
└── src/
```

接口使用 Engine-neutral port：

```cpp
namespace lux::ecs::integration
{
    class AssetResolver
    {
    public:
        virtual ~AssetResolver() = default;
        virtual AssetTicket request(asset::AssetId) = 0;
    };
}
```

Engine `AssetStore` 提供 Adapter。ECS Kernel 不知道缓存实现。

## 6. Reflection Sidecar 重构

### 6.1 类型所有者原则

每个组件目标拥有其组件描述与生成清单：

```text
ecs/core          → Name、Hierarchy、真正共享组件
ecs/transform     → Transform2D/3D
ecs/render        → Render components
ecs/physics3d     → Physics components
ecs/navigation    → Navigation components
```

不能把多个领域组件塞入 `ecs_meta` 只因方便。

### 6.2 Build-time 与 Runtime 分离

生成流程：

```text
组件头
→ build-time generator
→ generated metadata translation unit
→ 对应组件 sidecar/library
→ runtime catalog registration
```

ECS Core 只依赖生成后的 Runtime Reflection Interface，不依赖 generator executable。

### 6.3 Extension 动态加载

动态 Extension 加载后：

```text
ExtensionLoader 加载 DLL
→ Extension-owned metadata draft
→ validate against ComponentTypeCatalog
→ commit at safe point
→ ExtensionLease retained by installed schemas/systems
```

`ReflectionRegistry::drainPending()` 这种全局 pending chain 应逐步替换为显式 Draft；不能让动态库静态构造器隐式修改进程全局状态。

## 7. `SceneServices` 保留但内收

当前 `SceneServices` 的“装配期查询、运行时保存已解析依赖”方向正确。

规则：

- 只在 Scene/Feature 安装时 `find<T>()`；
- `ISystem::update()` 不得每帧查询；
- 必需服务解析后保存引用或专用 Client；
- 动态卸载服务使用 generation-aware `ServiceHandle<T>`；
- 不暴露 `Engine` 或通用 `get<T>()`。

可以更名：

```text
SceneServices → SceneServices（可保留）
```

因为其作用域和语义明确，不必为了统一命名而改。

## 8. Scene Format 与 Scene Package 拆分

### 8.1 当前混合

`modules/resource/entity_scene` 同时包含：

```text
Entity Section wire format
Persistent Entity identity
Component schema requirement
Required Extension
Scene Contribution
Startup sections
Generated section source
Persistence journal
```

前半属于 ECS，后半属于 Engine Scene 产品协议。

### 8.2 ECS Scene Format

CREATE：

```text
ecs/scene_format/
├── include/lux/ecs/scene_format/
│   ├── EntityId.hpp
│   ├── SectionId.hpp
│   ├── ComponentRecord.hpp
│   ├── SectionImage.hpp
│   ├── SectionCodec.hpp
│   └── Validation.hpp
├── src/
└── test/
```

目标内容：

```cpp
struct SectionImage final
{
    SectionId id;
    std::vector<EntityRecord> entities;
};

struct EntityRecord final
{
    PersistentEntityId id;
    std::vector<ComponentRecord> components;
};

struct ComponentRecord final
{
    ComponentSchemaId schema;
    std::uint32_t schemaVersion;
    std::vector<std::byte> payload;
};
```

该格式不知道：

```text
ExtensionId
SceneFeatureId
startup scene
game pack
render feature
world partition strategy
```

### 8.3 Engine Scene Package

CREATE：

```text
engine/scene/package/
├── include/lux/engine/scene/package/
│   ├── ScenePackage.hpp
│   ├── SceneFeatureRequest.hpp
│   ├── SectionSource.hpp
│   └── ScenePackageCodec.hpp
└── src/
```

目标：

```cpp
namespace lux::engine::scene::package
{
    struct RequiredExtension final
    {
        extensions::ExtensionId id;
        extensions::VersionRange version;
    };

    struct FeatureRequest final
    {
        scene::FeatureId id;
        std::uint32_t configVersion{};
        std::vector<std::byte> config;
    };

    struct ScenePackage final
    {
        SceneId id;
        std::vector<ecs::scene_format::SectionId> startupSections;
        std::vector<SectionSource> sections;
        std::vector<RequiredExtension> requiredExtensions;
        std::vector<FeatureRequest> features;
    };
}
```

### 8.4 Toolchain 依赖

`engine/toolchain/entity_scene` 改为：

```text
依赖 ecs/scene_format 生成 SectionImage
依赖 engine/scene/package 生成 ScenePackage
```

不再依赖一个 Resource 万能 Manifest。

## 9. Runtime Entity Loading 拆分

当前 `engine/runtime/entity_scene` 包含：

```text
EntityBatchDecoder
EntityBatchMaterializer
EntityBatchStager
EntitySceneCatalog
EntitySectionGeneratorCatalog
EntitySectionLoaderSystem
EntitySectionService
SectionBlobStore
StartupSectionSystem
```

目标归属：

| 当前类型 | 目标 |
| --- | --- |
| `EntityBatchDecoder` | `ecs/scene_format/SectionDecoder` |
| `EntityBatchMaterializer` | SUPERSEDED：纯 LXES image + Runtime `EntityBatchStager` 已提供 staged materialization |
| `EntityBatchStager` | `engine/scene/loading/SectionStager` |
| `EntitySceneCatalog` | `engine/scene/package/PackageCatalog` |
| `EntitySectionGeneratorCatalog` | `engine/scene/loading/SectionGeneratorCatalog` |
| `EntitySectionLoaderSystem` | `engine/scene/features/section_streaming/SectionLoaderSystem` |
| `EntitySectionService` | `engine/scene/loading/SectionLoader` |
| `SectionBlobStore` | `engine/scene/loading/SectionStore` |
| `StartupSectionSystem` | `engine/scene/features/startup_sections/StartupSectionSystem` |

解码纯函数下沉到 ECS Format；异步 I/O、AssetStore、Feature 与 System 留在 Engine。

## 10. Scene Feature 与 ECS System

### 10.1 所有权

```text
FeatureCatalog     保存 Feature Descriptor/Factory
Scene::Features    拥有已安装 Feature 状态
Schedule           唯一拥有具体 ISystem
SceneServices      拥有场景服务
```

Feature 安装事务：

```text
resolve feature dependencies
→ create unpublished services
→ create unpublished systems
→ validate service and schedule constraints
→ commit services
→ commit systems
→ publish active feature
```

失败必须全回滚。

### 10.2 Feature 不是 ECS Core

Physics3D、Navigation3D、Presentation3D 等 Feature 描述属于 Engine Scene Feature 层；具体 `PhysicsSystem` 等属于 ECS 领域目标。

## 11. `Schedule` 保留的机制

以下实现原则不可破坏：

- `Schedule` 保存 `std::unique_ptr<ISystem>`；
- System Handle 带 owner identity 与 generation；
- prerequisite/before/after 在拓扑编译时验证；
- 安装和移除只在安全点提交；
- `SystemUpdateContext` 只暴露 Registry、Command Writer、delta/tick；
- Command barrier 仍然唯一；
- 关闭按逆拓扑顺序。

重构只调整路径和外部装配，不将 Runtime/Asset/Extension 注入 `SystemUpdateContext`。

## 12. CMake 修改

### 12.1 `ecs/core/CMakeLists.txt`

目标 PUBLIC 依赖改为：

```cmake
target_link_libraries(core
    PUBLIC
        EnTT::EnTT
        lux::cxx::algorithm
        lux::cxx::compile_time
        lux::math
)
```

根据实际需要可加入纯 Reflection Runtime；移除：

```text
core::extension_abi
resource::asset_core
resource::entity_scene
resource::spatial
```

### 12.2 新目标

```text
lux::ecs::core
lux::engine::ecs::component_archive
lux::ecs::scene_format
```

`component_archive` 与 `scene_format` 可独立安装；不创建空泛 Asset Integration 或 RegistryArchive component。

## 13. 格式迁移

### 13.1 保持旧 LXES/LXSC 读取

第一阶段：

```text
Legacy EntityScene Codec
→ decode legacy manifest
→ split into ScenePackage + Section descriptors
```

第二阶段再写新格式。不能在目录迁移 PR 同时改变 wire image。

### 13.2 Golden Files

保存：

```text
空 Scene
单 Section
多依赖 Section
未知 Component
未知 Extension
压缩 Section
Generated Section
Persistence Journal
```

并验证：

```text
旧 Reader
新 Adapter
新 Reader（格式升级后）
```

## 14. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| ECS-01 | ComponentSchemaId 与 Registry 回迁 | Core 不依赖 extension_abi/meta entity |
| ECS-02 | Asset integration 移出 Kernel | Core 不依赖 Asset Core |
| ECS-03 | `resource/spatial → math` 调用迁移 | Core 不依赖 Resource Spatial |
| ECS-04 | 创建 `ecs/scene_format`，迁移纯 Codec | 旧 wire 可读 |
| ECS-05 | 创建 `engine/scene/package`，拆 Manifest | RequiredExtension 不在 ECS |
| ECS-06 | Runtime Entity Loading 按纯函数/Engine 拆分 | System 行为不变 |
| ECS-07 | Reflection static pending 改 Draft Commit | Dynamic load/unload 测试通过 |
| ECS-FINAL | 删除旧 Resource EntityScene targets | 依赖图符合合同 |

## 15. 验收闸门

- [ ] `ecs/core` 不依赖 `engine/*`。
- [x] `ecs/core` 不依赖 Extension ABI。
- [x] `ecs/core` 不依赖 AssetManager/AssetStore。
- [x] Entity Registry 位于 ECS。
- [ ] ComponentSchemaId 位于 ECS。
- [ ] ECS Scene Format 不包含 RequiredExtension 或 SceneFeature。
- [ ] Engine Scene Package 不进入 modules/resource。
- [ ] `Schedule` 仍唯一拥有 System。
- [ ] `SystemUpdateContext` 未新增 Runtime/Asset/Extension getter。
- [ ] 旧 Entity Scene/Section Golden Files 全部可读。
- [ ] 动态 Extension metadata 使用显式 Draft/Commit，不依赖隐式全局 pending chain。
