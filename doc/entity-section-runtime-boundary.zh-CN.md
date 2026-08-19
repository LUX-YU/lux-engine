# EntitySection 运行时边界

## 决议

纯 LXES EntitySection 数据、标识、附件引用、持久化日志以及确定性编解码属于 `ecs/scene_format`。Extension 要求、Scene Feature 请求、Section 来源配方和启动策略属于 Engine `ScenePackage`。`modules/resource/entity_scene` 只保留为旧 LXSC/LXES v1 的冻结兼容实现，不能继续充当运行时领域模型。

## 所有权

```text
ecs/scene_format
├── EntitySectionId
├── EntitySectionImage
├── EntitySectionAttachment
├── ContentBlobId / ContentBlobRef / ContentTypeId
├── PersistenceJournal
└── LXES encode / decode / validation

engine/scene/package
├── ScenePackageId
├── RequiredExtension
├── SceneFeatureRequest
├── SectionRecord
├── StoredSectionSource / GeneratedSectionSource
└── LXSC encode / decode / validation

engine/runtime/entity_scene
├── Decode admission
├── private-registry staging transaction
├── ECS materialization and retirement
├── content-blob lease ownership
└── startup Section residency
```

Runtime 层消费上述两个规范所有者，但不重新定义 wire 数据结构。Toolchain 直接构造 `EntitySectionImage` 与 `ScenePackage`，不经由旧 Resource DTO 绕行。

## 兼容边界

迁移期只允许以下位置理解旧 `lux::entity_scene` 类型：

- `modules/resource/entity_scene/**`：冻结的 v1 实现；
- `engine/scene/package/src/LegacyEntitySceneAdapter.*`：LXSC 新旧模型转换；
- Scene Package 与 EntitySection 字节兼容测试；
- `Spatial3DEntitySceneAdapter.cpp` 中针对尚未归位的 `resource/spatial3d_scene` 格式的局部 ID 转换。

兼容转换必须显式按稳定名称或 UUID value 进行。禁止给新旧类型增加隐式构造函数、通用 conversion helper 或兼容 overload。

## 公共 API 规则

`engine/runtime/entity_scene` 的公开头只能暴露：

```cpp
lux::ecs::scene_format::*
lux::scene::ScenePackage
lux::scene::SectionRecord
```

不得暴露：

```cpp
lux::entity_scene::EntitySectionImage
lux::entity_scene::EntitySectionRecord
lux::entity_scene::ContentBlobRef
lux::entity_scene::PersistenceJournal
```

运行时类名中的历史 `EntityScene` 暂时保留到目录迁入 `engine/scene/loading` 的独立阶段；这不是旧 DTO 继续拥有运行时语义的许可。

## 构建边界

- `runtime_entity_scene` 的生产目标必须 PUBLIC 链接 `ecs::scene_format` 与 `scene::scene_package`；
- 生产目标不得链接 `resource::entity_scene`；
- 旧 Resource 组件只能出现在字节兼容测试与 ScenePackage 私有 Adapter 的 PRIVATE 闭包；
- `toolchain_entity_scene_cook` 不得依赖旧 Resource 组件。

## 自动检查

`tools/architecture/check_entity_section_boundary.py` 检查：

- Runtime 与 canonical cooker 是否重新 include 旧 Resource 头；
- 运行时公开类型是否泄漏旧 EntitySection/Blob/Persistence 类型；
- CMake 生产闭包是否重新链接旧 Resource 组件；
- Spatial3D 临时适配例外是否扩大。

删除 `modules/resource/entity_scene` 之前，还必须完成 Authoring、Game Export、旧 Spatial3D 配置和 Golden Files 的迁移。建立新所有者与删除兼容实现是两个独立验收项。
