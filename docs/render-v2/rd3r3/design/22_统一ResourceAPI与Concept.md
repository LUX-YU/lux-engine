# 22 — 资源 API 一致性不等于资源数据结构同构

## 22.1 资源作者看到的最小接口

用户要求 `getOrCreate<T>()` camelCase。这个 API 应定位于**后端冷态 ScopeResources/Factory**，而不是 RenderEffect 或每帧 Executor 自己查找资源。ScopeKey 由 Backend 在已经确认 Scene/View/Effect/Target 之后创建；用户不必为了写 GridFeature 先查询 DeviceSession 的 DomainId。

```cpp
// 目标语义：class ScopeResources 也可只是 DeviceSession 内部轻量视图
const auto scope = makeScopeKey(ResourceScope::SCENE, validatedSceneId);
auto& resources = session.scopedResources(scope);
auto value = resources.getOrCreate<LightStorage>("lights.main", lightFactory);
// 借用者通过 attachment lease 保活；不能把 value 的 T& 存到 Effect 作者对象中
```

## 22.2 操作语义

| 操作 | 唯一合法语义 |
|---|---|
| `tryGetLocal<T>(key)` | 仅当前已验证 Scope 查找，返回允许范围内借用，不能用于跨资源 Owner 更改 GPU allocation |
| `tryGet<T>(scope,key)` | 在明确设计允许的只读 fallback/scope 查询中提供观察，不通过隐式父链偷改祖先 |
| `getOrCreate<T>(key,factory)` | 本 scope 条目已有时调用 `factory.isCompatible(existing)` 但不调用昂贵的 `factory()`；不存在时构建完成再发布 |
| `getOrCreate<T>(key,std::move(owned))` | 接受已构造好的唯一 owner；已占用必须清晰冲突或保持旧语义，绝不悄悄覆盖 |
| `acquireLease(key)` | 成功 attach/preload 使用权，严格保证 Native backing 在使用者存活期间不被回收 |
| `releaseLease` | 引用减一后依 retention 保留/释放；GPU 物理资源还要按 ticket 退休 |
| `replace(key,newConfig)` | 显式版本化替换事务，不隐含于 getOrCreate 或 per-frame 数据值更新 |

## 22.3 最小 Concept 草案

```cpp
template<class Factory, class T>
concept ResourceFactoryFor = requires(Factory f, const T& existing) {
    { f() } -> std::same_as<RenderResult<std::unique_ptr<T>>>;
    { f.isCompatible(existing) } -> std::same_as<bool>;
};

template<class Storage>
concept GraphBindingProvider = requires(const Storage& s, ResourceSemanticKey key) {
    // Exact return type remains backend-specific; `describeBinding` must return
    // owned/cold metadata or valid borrow of Native backing, not an owning copy.
    { s.describeBinding(key) };
};
```

本地 C++20 约束足以阻止半有效的 Factory 和重复 API；不强制所有 GPU Storage 都实现虚拟 `IResource` 接口，也不要求 `MeshStorage::update` 与 `GridData::update` 同参数。插件 API 通过 ABI-safe callback table，不能暴露 RTTI + owning STL。

## 22.4 Storage 键、兼容性与写权

条目身份 = DeviceSession + ScopeKey + Type canonical name/version + ResourceKey + GPU Layout/Config compatibility。TypeId 哈希必须保留原名 Collision 校验。多同型资源需要 key（如 `Light.main` 与 `Light.preview`）；Config 不匹配返回可恢复的 DefinitionMismatch；类型/Shader ABI 冲突需拒绝，而不是自动增长或调用另一 Feature 的 create()。

Asset 写入和 Attach 使用同一个 Storage Owner：CPU source update 只修改自身授权的 typed slice/dirty log；需要按 GPU-queue time 与真实 in-flight ticket 协调 descriptor/Buffer 替换。已验证的 binding recipe 由 `PreparedBindings` 保存，热态不再执行类型查找或 hash map 解析。

## 22.5 当前 P2-A 代码迁移

P2-A `ResourceDomain::tryGet()/getOrCreate` 的候选构造、命中不重复建立、地址稳定策略可以复用，但那个 Public `ResourceDomain` 类的完整层级树与显式 DomainId 不是最终用户层必须保留的类型。先沿用其 Native Buffer Factory 证明 ScopeResource/Lease/attach，再以具体 Consumer 和 GPU 使用权测试决定如何合并/删除旧 Domain，而不是创建 ScopeResourcesManager/ResourceDomainManager 两层平行权威。
