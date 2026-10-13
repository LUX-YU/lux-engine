# 15 — 端到端目标 API：有意区分用户、后端与资源作者

> 下面均为目标伪码；**不代表 P2-A HEAD 已有这些签名或完整功能**。真正实现时优先遵循现有 R2 message/reply，无阻塞本机 C++ direct call 偷穿 Engine 边界。

## 15.1 Engine/Editor 只依赖 Client

```cpp
// 由 Application/Native Bootstrap 完成选 GPU 和 Vulkan/D3D12 Native Provider 装配
// Engine ECS / Editor 只获得 neutral RenderRuntime& client
RuntimeScene scene = await(client.createScene());
RuntimeView view = await(client.createView(scene));
RuntimeTarget target = await(client.createTarget(targetDescription));
RuntimeEffect effect = await(client.createEffect(deferredEffectDescription));
await(client.attachEffect(effect, view, target, placement));
// View 本身不存 effectId/graphId/targetId；Effect 不存 sceneId/viewId
```

上层在应用启动时选择 Native Provider/设备；Engine Rendering、ECS/Project UI 不调用 `backend.assemblyDevice()`、不包含 Vulkan 头。`await` 仅表示使用现有 request/reply 机制取结果，不是新增异步线程 API。

## 15.2 Effect 通过 Feature 形成 Plan

```cpp
struct GridSettings { float spacing{}, opacity{}; };
struct GridFeature {
    static void describe(const GridSettings&, ResourceRequirements& out);
    static RenderResult<void> build(const GridSettings&, RenderGraphBuilder&) noexcept;
};
// ResourcePlan 在 compile 阶段形成；此时没有某 Scene 的 VkBuffer
// 具体 GridData 可以来自 ECS/Editor 插件，Plan 只持 neutral type/key/scope
```

## 15.3 attach 内部使用统一资源 API

```cpp
// Native owner-thread 示例：由已验证的 ScopeKey + ResourceSlotKey 获得 scoped resource
// 具体 public facade 不需要向 Feature/Engine 暴露
ScopeKey scope = resolveScope(attachment, requirement);
auto& scopeResources = session.scopedResources(scope);
auto storage = scopeResources.getOrCreate<MeshStorage>(resourceKey, lazyMeshFactory);
if (!storage) return cxx::unexpected(storage.error());
// acquireLease(*storage) 与 PreparedBindings 一起组装成 Backend-owned Attachment
// executor 不会重复调用 session.scopedResources()/getOrCreate()
```

`getOrCreate<T>(factory)` 只能在缺失时调用昂贵 factory；同 T key 既存但不兼容要 error，不可 silently rewrite；`getOrCreate<T>(std::move(owned))` 表示明确接管完整 Native owner 的另一种形式。这里用 ScopeKey，而不是根据 SceneId 猜 DomainId。

## 15.4 同一个 View 多个 Effect / 私有 History

```cpp
await(client.attachEffect(gridEffect, view, editorTarget));
await(client.attachEffect(tsrEffect, view, gameTarget));
// Camera facts 可在同 View scope；TSR history 按 (effect,scene,view,target,signature) 私有
await(client.detachEffect(tsrEffect, view, gameTarget));
// gridEffect 的 attachment 与该 View 仍有效
```

## 15.5 ECS 与插件 DataProjection

```cpp
struct GridProjection {
    static constexpr auto observations = observe<GridComponent>();
    static RenderResult<void> project(
        const GridComponent& component, RenderDataWriter<GridData>& out) noexcept;
};
// Engine/Editor 插件注册这个 projection，并且只负责中立 typed payload；
// 不知 Vulkan Native Buffer/Graph/Binding 的实际布局。
// `RenderDataWriter` 为目标语义示例：Wire/Owner/backpressure 应复用现有 R2 管线。
```

## 15.6 一 Target 多 Effect 的完整输出

```text
Effect A logical graph + Effect B logical graph + Target layers
    → compiler-derived composite graph (no second editable logical owner)
    → Native Graph variant for full Target final output
    → PreparedBindings/Resources/ExecutionContext from all participating attachments
    → ONE native output operation; WSI present follows its valid GPU completion
```

## 15.7 设备作用域

```text
Vulkan bootstrap:
  owns InstanceContext; enumerates physical GPUs; selects one
  constructs DeviceSession borrowing InstanceContext (one selected logical device)
  adopts native session through neutral backend port
Server RenderBackend:
  owns scenes/effects/targets/attachment triples, never gives Engine a VkDevice
```
