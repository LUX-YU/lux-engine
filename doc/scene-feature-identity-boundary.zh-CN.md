# Scene Feature 身份边界

## 目的

`ContributionId` 是旧 Extension ABI 为多种贡献类型提供的通用标识。它曾同时表示 Scene Feature、Render Effect、Editor Panel 以及旧 LXSC 文件中的 feature selection，导致完全不同的领域值可以被误传给同一 API。

运行时 Scene 能力现在只使用：

```cpp
lux::scene::SceneFeatureId
lux::scene::SceneFeatureIdView
lux::scene::sceneFeatureId(...)
```

该类型与 `ExtensionId`、旧 `ContributionId`、`RenderEffectId` 和 `PanelId` 均不可互换。

## 当前边界

```text
旧 LXSC v1 wire / legacy DTO
    lux::extensions::ContributionId
                 │ canonical name conversion
                 ▼
Engine Scene / Runtime Scene Feature
    lux::scene::SceneFeatureId
```

旧 wire 仍保留原字段和字节编码，以维持格式兼容。转换只允许发生在：

- `engine/scene/package/src/LegacyEntitySceneAdapter.cpp`；
- 仍负责读取旧 LXSC DTO 的受控 Runtime 边界。

Scene Feature Catalog、动态启停命令、Ticket、Snapshot、状态事件和产品调用点不得接收 `ContributionIdView`。

## 已实施的编译约束

`SceneContributionCatalog`、`SceneContributions` 和 Scene Feature owner 已删除 `ContributionIdView` 兼容重载。旧类型传入这些 API 会在编译期失败，而不是在运行时按字符串碰运气。

`EntitySceneCatalog::findContribution()` 虽然仍从旧 LXSC DTO 返回记录，但查询参数已经是 `SceneFeatureIdView`。这是 wire DTO 与 Runtime 语义之间的显式边界。

`tools/architecture/check_scene_feature_identity.py` 会拒绝非 legacy wire 代码重新引入：

```text
lux::extensions::contributionId(...)
ContributionIdView legacy_id
failure.contribution
```

该检查在 `architecture-recovery` 工作流中执行。

## 后续删除闸门

只有满足以下条件后，才能删除旧 `ContributionId`：

1. `modules/resource/entity_scene` 的所有消费者迁移完成；
2. 新 Scene Package codec 成为唯一 Engine 入口；
3. 旧 LXSC reader 被限制为私有兼容 Adapter；
4. Extension ABI 中不存在其他领域借用 `ContributionId`；
5. Golden tests 证明旧 LXSC 文件仍可读取并生成相同语义。
