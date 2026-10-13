# 24 — Engine/ECS → Render 的 DataProjection，不是 Feature 专用 Proxy

## 24.1 问题定位

GridFeature 需要 GridData；Engine 可能维护自己的 `GridComponent`、Editor Viewport 自有设置、robot sensor 数据也可能来自网络。Renderer/Shader 不应 include ECS Component 也不应知道 EnTT 或某具体 Asset 类型。**Engine 只观察业务事实并按中立 DataSchema 发布；Render Native 消费中立数据。**

## 24.2 值得继承的 Legacy 算法

Legacy `RenderFeatureSceneBinding` 支持特定 SceneSystem/Feature 的 `observations` 与 `create_sync_stage` 注册；`RenderSyncStage` 持有 `prepare/commitPrepared/discardPrepared`，`Builtin3DRenderStages` 的 `TExtractionChangeSet`/Component leave observer 能够 O(changed) 记录增量和资源离开；RenderSystem 在完整 PROGRAM 被接受后才 commit prepared source state。它们是有价值的**反应式观察与背压事务机制**，不能全部砍掉改每帧扫描 ECS。

但不需要恢复每 Feature 一份继承 `RenderSyncStage` 的长期 Proxy；新架构用注册型 `DataProjection` 描述输入组件/事件与输出数据的类型和生成函数，相关 Producer/Observer 的寿命属于安装它的 Engine/Editor Plugin 模块。核心 RenderSystem 不为每个 Feature 写 `case GRID`/`case MESH`，也不默认 Feature 自己拥有 ECS Registry。

## 24.3 统一投影过程

```text
Source ECS Component / Asset / Editor Grid Settings / External Sensor
 → registered ProjectionDescriptor {observes, produces DataSchema, Scope, authority,
                                     conversion ops, code pin}
 → typed change observation O(changed), entity leave/create/replace
 → prepare logical values/delta + neutral Wire packet (version/generation)
 → R2 PROGRAM/UPLOAD admission; accept → commitPrepared; backpressure → retain;
    cancel → discardPrepared without losing source-of-truth
 → Backend validates typed payload/ScopeKey/Writer Authority
 → if Storage active, patch typed dirty data + GPU Upload/Ready; if not attached,
    keep declared latest source revision/snapshot or request a cold full-sync on attach
 → Native attachment PreparedBindings reference correct Storage version
```

`ProjectionDescriptor` 可以是值/固定函数 Ops：`observes` 包含 ECS Component schema ID、必要/可选关系；`produces` 是 neutral DataSchema canonical name/version、合法 Scope/Key；Host 在安装时冷态解决 Producer 依赖和唯一 writer policy。插件 callback 需要 CodePin，安装后只调用已有固定 ops，不做每帧 FeatureTypeId 字符串查找。

## 24.4 Grid 示例

`GridData {spacing, majorWidth, minorWidth, opacity, origin}` 是渲染中立类型，GridFeature 构图时要求 View/Attachment 中的 `GridData`，Shader 使用验证后的 GPU Representation。Engine GridComponent/Editor UI 到 GridData 的转换由 `GridProjection` 实现并注册；若 Grid 只属于 Editor 设置，可以绕过 ECS，直接由 Editor Producer 使用相同 DataSchema 发布。**不会为了统一而强制每个 Editor 参数都做一个 ECS Component。**

## 24.5 Mesh/Light/Gaussian 示例

- `Mesh3D+WorldTransform+ResolvedAsset` → `MeshInstanceData`/resource deltas；稳定 Entity handle 必有 World/Scene+generation，不把裸 ECS bits 长期当 GPU stable slot；资产复用进入 Global MeshStorage，实例数据进入 Scene Scope。
- `LightComponent` → `LightGpuData` 及 Scene LightStorage 增量，多个 Effect 消费同一 Scene data；光源更新不触发图编译。
- 插件 `GaussianComponent`/external SLAM → `GaussianChunkData` GPU upload，Native Gaussian storage 可以由 Plugin 提供，但 Plugin 不得让 RenderBackend 内置识别其 C++ Component 类型。

## 24.6 背压与多 Writer

Producer 维护 last committed revision 和 pending delta；R2 transport `BACKPRESSURED` 不提交已准备 ECS 变更，保证后续重试只发送真实缺失的更新。Entity leaves/clearAll 的顺序与 DataVersion 被正确保留，删除之后新建相同 Entity ID 要 generation 区分。多个 Producer 写同一个 Scoped DataKey 需要显式 merge/partition/priority policy；不能最后注册/最后 publish 自动覆盖。

## 24.7 插件与可安装 SDK 验收

独立安装 Plugin 贡献 GridFeature/GridData/GridProjection 后，Engine 运行中的 ECS/Editor 修改通过 neutral client 形成 Native GPU 可验证变化；测试 Create/Modify/Remove、Backpressure、Project switch/Plugin unload/CodePin，且 Engine core 业务代码没有新增 case 分支。Wire Schema/Version/Host C ABI 值全链一致，无 RTTI/owning STL 跨 DLL。
