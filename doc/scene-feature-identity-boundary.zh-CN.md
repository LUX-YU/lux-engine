# Scene Feature 身份边界

## 目的

`ContributionId` 是旧 Extension ABI 为多种贡献类型提供的通用标识。它曾同时表示 Scene Feature、Render Effect、Editor Panel 以及旧 LXSC 文件中的 feature selection，导致完全不同的领域值可以被误传给同一 API。

运行时 Scene 能力现在只使用：

```cpp
lux::scene::SceneFeatureId
lux::scene::SceneFeatureIdView
lux::scene::sceneFeatureId(...)
```

该类型与 `ExtensionId`、`RenderEffectId` 和 `PanelId` 均不可互换；旧通用
`ContributionId` 已删除。

## 当前边界

```text
LXSC v1 wire（stable name/hash）
                 │ Scene-owned bounded codec
                 ▼
Engine Scene / Runtime Scene Feature
    lux::scene::SceneFeatureId
```

LXSC v1 仍保留原字段和字节编码；wire 不编码 C++ 类型名。Engine Scene 原生 codec
直接构造 `SceneFeatureId`，不再存在 legacy DTO 或 `LegacyEntitySceneAdapter`。

Scene Feature Catalog、动态启停命令、Ticket、Snapshot、状态事件和产品调用点不得接收 `ContributionIdView`。Render Effect 对 Scene Feature 的依赖列表同样必须拥有 `SceneFeatureId`，不能借用 Extension `ContributionId`。

## 已实施的编译约束

`SceneContributionCatalog`、`SceneContributions` 和 Scene Feature owner 已删除 `ContributionIdView` 兼容重载。旧类型传入这些 API 会在编译期失败，而不是在运行时按字符串碰运气。

`EntitySceneCatalog`、Scene Package 与 Runtime 查询均只接受领域 ID。通用
`ContributionId` 及其 helper 已从生产、测试和 CMake 删除。

历史 `tools/architecture/check_scene_feature_identity.py` 已按项目裁决退役。当前防回流由
owner 编译/链接契约、LXSC Golden/Wire tests、Extension ABI tests 与 CMake DAG 承担，
并持续扫描以下旧模式：

```text
lux::extensions::contributionId(...)
ContributionIdView legacy_id
failure.contribution
std::vector<lux::extensions::ContributionId>
```

## 删除结论

`modules/resource/entity_scene`、旧 Scene Package 兼容层和通用 `ContributionId` 均已删除；
Engine Scene 原生 codec 是唯一入口。LXSC v1 Golden 仍可读取并逐字节重编码，因此该源码
清理没有升级或改变 wire。
