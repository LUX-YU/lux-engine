# Engine 执行、资产、场景与扩展重构

> 纯化 Engine 的 Execution、Scene、ExtensionLoader 与产品适配边界，并复用公共资产机制

**执行文档 06 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Engine Execution、Asset、Scene、Extension、Render Integration、Spatial 与关闭协议负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** 不建立 `engine/assets` 或 `AssetStore`。`engine/runtime/assets` 保留为现有公共 `AssetManager` 的异步编排适配器；`engine/scene` 收敛成拥有 `SceneDescription`、`SceneAsset` 与 `SceneAssetSerDeser` 的单一组件，不拥有 IO、异步执行或 Registry 生命周期。本文后续与此冲突的目标目录和接口由 `ADR-20260820_SceneAsset与Resource边界.md` 取代。

> **2026-08-21 裁决更新：** 新建的 `engine/content` 只拥有冻结内置资产 UUID、M_Missing 与色板，不是第二套资产系统。Runtime Render 在唯一装配点把 fallback material ID 注入 ECS Residency。Toolchain 只拥有 Pak cook/publish 策略，不访问 Resource 私有 Pak wire 实现。

> **2026-08-21 加载裁决：** `engine/runtime/assets` 通过既有 `AssetLoadService` 编排 IO、manager-less SerDeser decode 与主线程安装。Runtime packs/Scene Script integration 直接表达需求；不得向 ECS 注入裸加载函数或在 tick 中调用同步 `ensureAsset()`。


## 1. 目标目录

`engine/runtime` 当前承担“所有运行期代码”的物理聚合，但其子目录已经是独立领域。目标是去掉这一中间层，直接按语义组织：

```text
engine/
├── execution/
├── extensions/
│   ├── api/
│   ├── loader/
│   ├── registration/
│   └── reflection/
├── scene/
├── render/
│   └── scene_bridge/
├── spatial2d/
├── spatial3d/
├── spatial_partition/
├── logging/
├── game/
├── authoring/
├── toolchain/
└── editor/
```

| 当前目录 | 目标目录 | 核心处理 |
| --- | --- | --- |
| `engine/runtime/execution` | `engine/execution` | `AsyncRuntime → Executor`；保持 Engine 内部 |
| `engine/runtime/assets` | 保留 | 既有 `AssetManager` 的异步编排适配器，不复制资产系统 |
| `engine/runtime/extensions/loader` | `engine/extensions/loader` | `ExtensionModuleManager → ExtensionLoader` |
| `engine/runtime/extensions/contribution_host` | `engine/extensions/registration` + 各领域 Catalog | 拆除 `EngineExtensions` 聚合 |
| `engine/runtime/entity_scene` | 保留 Runtime loading 职责 | LXES 纯格式已归 `ecs/scene_format`；不并入 Scene Codec 组件 |
| `engine/runtime/scene/core` | 保留 Runtime 生命周期职责 | `engine/scene` 只提供 Scene Asset 数据与同步 SerDeser |
| `engine/runtime/scene/script` | `engine/scene/features/script` + `engine/game/Session` | Feature 与会话入口分开 |
| `engine/runtime/render/backend_host` | `modules/function/render` | `RenderBackendHost → render::Renderer` |
| `engine/runtime/render/scene` | `engine/render/scene_bridge` | ECS/Scene 到 Renderer 的集成 |
| `engine/runtime/frame` | 产品私有 `Game::Frames` / `Editor::Frames` | 不再安装为公共 Runtime Component |
| `engine/runtime/logging` | `engine/logging` | Engine async sink；公共 log 不依赖它 |
| `engine/runtime/packs` | `engine/scene/features/*` 或 `engine/game/standard_features` | 不再用 Pack 表示通用能力 |
| `engine/runtime/spatial*` | `engine/spatial2d`、`engine/spatial3d`、`engine/spatial_partition` | World Partition 与准备服务归领域 |

## 2. Execution：保留在 Engine，先纯化再评估

### 2.1 不立即下沉 modules

当前 Execution 具有：

```text
MainThreadMailbox
MainThreadScheduler
Engine close admission
Dynamic Operation Bundle
Extension Module Lease
owner-thread completion
```

这些是 Engine 语义。仅因为 Game 与 Editor 共用，不能把它下沉到公共 Core。

### 2.2 类型迁移

```text
lux::exec::AsyncRuntime          → lux::engine::execution::Executor
AsyncRuntimeBuilder              → ExecutorBuilder
AsyncOperation                   → Operation
AsyncOperationClient             → OperationClient
AsyncOperationBundle             → OperationBundle
AsyncScope                       → TaskGroup
AsyncStatistics                  → ExecutorStats
AsyncFileService                 → FileIO
MainThreadMailbox                → detail::MainThreadMailbox
MainThreadStateCache             → detail 或明确领域 Cache
```

### 2.3 目标接口

```cpp
namespace lux::engine::execution
{
    class Executor final
    {
    public:
        static expected<Executor, OpenError>
        open(ExecutorConfig, MainThreadId);

        template<Operation Op>
        OperationClient<Op> client() const;

        MainThreadScheduler mainThread() const noexcept;
        TaskGroup makeTaskGroup();

        void drainMainThread(std::size_t budget);
        CloseTask close();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
```

不提供：

```cpp
template<class T> T& get();
void* service(std::type_index);
```

### 2.4 Operation 注册

Engine 内置 Operation 在组合根创建 `ExecutorBuilder` 时注册。动态 Extension 使用专用 `ExtensionRegistrar::operations()` 注册未发布 Operation Bundle。

注册流程：

```text
Extension callback
→ create unpublished OperationBundle
→ validate dependencies/queue capacity
→ install on Executor coordinator
→ publish remaining domain descriptors
```

保留当前“先安装异步 Bundle、再提交 Catalog”的安全顺序，但命名与所有者归位。

### 2.5 `EditorAsyncService` 不进入 Engine Execution

Editor Operation 按领域归属：

```text
import              → editor::Content
thumbnail           → editor::Thumbnails
material compile    → toolchain::MaterialCompiler
flow compile        → toolchain::FlowCompiler
scene cook          → toolchain::SceneCooker
model generation    → toolchain::ModelImporter
```

Executor 只执行，不聚合用例。

## 3. Engine AssetStore

### 3.1 合并当前实现

MOVE：

```text
modules/resource/asset/core/AssetManager.*
engine/runtime/assets/AssetLoadService.*
→ engine/assets/AssetStore.*
→ engine/assets/AssetLoading.*
```

公共 `lux::asset::Vfs/CodecRegistry/Provider` 作为构造依赖。

### 3.2 目标所有权

```cpp
class AssetStore final
{
public:
    static expected<AssetStore, OpenError>
    open(
        asset::Vfs&,
        asset::CodecRegistry&,
        execution::Executor&,
        AssetBudget);

    LoadTicket load(asset::AssetId);
    AssetHandle find(asset::AssetId) const noexcept;
    void release(AssetHandle);

    AssetStoreStats stats() const noexcept;
    CloseTask close();

private:
    struct State;
    std::unique_ptr<State> state_;
};
```

### 3.3 Handle

动态缓存对象不得向长生命周期消费者暴露裸指针：

```cpp
struct AssetHandle
{
    AssetSlot slot;
    std::uint32_t generation;
};
```

访问：

```cpp
const T* tryGet<T>(AssetHandle, asset::AssetTypeIdView expected) const noexcept;
```

跨异步任务输入使用不可变值快照或 `AssetLease`，不能捕获 `AssetManager*`。

### 3.4 Scene Asset Adapter

`SceneAssetServices.hpp` 改为明确 Adapter：

```text
engine/scene/integration/assets/SceneAssetResolver.hpp
```

它实现 Scene Feature 需要的窄接口，不将整个 AssetStore 放入 `SystemUpdateContext`。

## 4. Extension API 与 Loader

> `ADR-20260821_ExtensionAbiV4Owner与Core清零.md` 修订本节：ABI v4 的实体定义本轮归
> `engine/extensions/api` 并删除 Core 组件，但 ABI-facing registrar/draft/lease 名称和对象
> 布局不在 v4 内改名。以下 `ExtensionRegistrar/ExtensionDraft/ExtensionLease` 是未来 ABI
> 版本或纯宿主内部重构方向，不得作为 v4 owner 搬迁的验收条件。

### 4.1 不创建 `Modules`

产品组合根拥有：

```cpp
extensions::ExtensionLoader extensions;
```

这只是动态扩展加载与卸载所有者，不是所有服务的容器。

### 4.2 文件迁移

MOVE：

```text
modules/core/extension_abi/*
→ engine/extensions/api/*

engine/runtime/extensions/loader/ExtensionModuleManager.*
→ engine/extensions/loader/ExtensionLoader.*

engine/runtime/extensions/loader/ModuleLifetime.hpp
→ 后续宿主内部整理；v4 registrar 可见布局暂不改名

engine/runtime/extensions/contribution_host/RuntimeContributionRegistrar.*
→ v4 ABI-facing surface 保留名称；后续新 ABI 再评估改名

EngineExtensions.*
→ 拆除
```

### 4.3 Extension Registrar

一个 Engine Extension 入口可以接收一个专用 Registrar，但 Registrar 只收集领域描述，不提供任意服务查询。

当前 ABI v4 的 canonical 类型仍为 `RuntimeContributionRegistrar` 与
`EditorContributionRegistrar`。下面的 `ExtensionRegistrar` 仅是未来版本草图，不在本轮创建。

```cpp
class ExtensionRegistrar final
{
public:
    ecs::ComponentSchemaRegistrar& components() noexcept;
    scene::FeatureRegistrar& sceneFeatures() noexcept;
    render::ExtensionRegistrar& render() noexcept;
    execution::OperationRegistrar& operations() noexcept;

    ExtensionDraft finish() &&;
};
```

关键区别：

- `ExtensionRegistrar` 位于 Engine Extension SDK，语义明确；
- `SceneFeatureId`、`Render FeatureId`、`ComponentSchemaId` 分别由领域定义；
- 没有通用 `ContributionId`；
- 没有 `get<T>()`；
- 不创建 `engine::module::Modules`。

### 4.4 `EngineExtensions` 拆分

当前 `EngineExtensions` 同时编排 Loader、Catalog、Activation 与 Close。目标拆为：

```text
ExtensionLoader       动态库、descriptor、依赖图、lease
ExtensionDraft        未发布注册结果
ExtensionActivation   产品私有的一次提交过程
各领域 Catalog        Component / Scene Feature / Render / Operation
```

`ExtensionActivation` 可是 `.cpp` 内部函数，不必成为公共类型：

```cpp
expected<LoadedExtension, ActivationError>
activateExtension(
    ExtensionLoader&,
    ExtensionDescriptor,
    CatalogSet&);
```

### 4.5 卸载

卸载前检查：

```text
Extension-owned Operations 无 accepted work
Scene-owned Feature 已关闭
Schedule-owned Systems 已移除
Render-owned Feature/Effect 已关闭
Reflection metadata 无 live object
所有 ExtensionLease 归零
```

满足后才 unload DynamicLibrary。

## 5. Scene

### 5.1 `SceneRuntime → Scene`

MOVE：

```text
engine/runtime/scene/core/include/.../SceneRuntime.hpp
→ engine/scene/core/include/lux/engine/scene/Scene.hpp

SceneRuntime.cpp
→ Scene.cpp

SceneRuntimeCloseSender.hpp
→ CloseTask 或 SceneClose.hpp（内部）
```

目标：

```cpp
class Scene final
{
public:
    static expected<Scene, OpenError>
    open(SceneConfig, SceneDependencies);

    ecs::World& world() noexcept;
    ecs::Schedule& schedule() noexcept;
    Features& features() noexcept;

    void update(const Frame&);
    CloseTask close();

private:
    struct State;
    std::unique_ptr<State> state_;
};
```

`SceneDependencies` 是构造期结构体，只含精确引用：

```cpp
struct SceneDependencies
{
    execution::Executor& executor;
    assets::AssetStore& assets;
    events::Events& events;
    ecs::ComponentTypeCatalog& components;
    FeatureCatalog& features;
};
```

Scene 不保存一个通用 Engine Environment。

### 5.2 Features

当前：

```text
SceneContributionDescriptor
SceneContributionCatalog
SceneContributions facade
SceneContributionHost
BatchBuilder / Bootstrap / InstalledBatch
```

目标业务表面：

```text
FeatureDescriptor
FeatureCatalog
Features
FeatureHandle
```

事务细节放 `detail`：

```text
FeatureTransaction
ServiceBatch
SystemBatch
FeatureLease
```

### 5.3 Scene Feature 安装

```cpp
expected<FeatureHandle, FeatureError>
Features::enable(FeatureIdView id, std::span<const std::byte> config);
```

实现：

```text
lookup descriptor
resolve dependency closure
create services/systems unpublished
validate
commit SceneServices
commit Schedule
publish active feature
```

### 5.4 Scene Script

`SceneScriptRuntime` 当前只是 Scene+Script 的生命周期包装。拆分：

```text
脚本 ECS 行为 → ecs/script
Script backend → modules/function/script
Scene 安装脚本能力 → engine/scene/features/script
会话启动/停止入口 → engine/game/Session
```

删除单独 `SceneScriptRuntime` 类型。

## 6. Render Integration

### 6.1 `RenderBackendHost`

其真正职责是渲染线程、Channel、Server、Session 与 Backend 生命周期。为满足公共 Render SDK，MOVE 到 `modules/function/render` 并收敛为 `render::Renderer`。

Engine 不再拥有第二个 Renderer wrapper。

### 6.2 Scene Bridge

保留 Engine/ECS 到公共 Renderer 的适配：

```text
engine/runtime/render/scene
→ engine/render/scene_bridge
```

职责：

```text
ECS extraction
SceneView 生命周期
Entity/Component → Render Packet
AssetHandle → Render Upload
Scene close 与 View close 协调
```

不得把 `Renderer` 内部 Channel 暴露给 Scene。

### 6.3 `EditorRenderInfra`

删除宽泛裸指针包。调用者接收：

```text
render::Renderer&
render::SceneView&
render::OffscreenView&
assets::AssetStore&
```

中的精确子集。

## 7. Spatial 与内置 Feature

### 7.1 World Partition

MOVE：

```text
resource/spatial3d_scene
engine/runtime/spatial3d/partitioned
engine/runtime/spatial_partition
→ engine/spatial3d/world_partition
```

目录内部再分：

```text
format/        Engine Scene Package 的 feature config
runtime/       interest/residency/streaming
toolchain/     cook and catalog generation（实际代码留 toolchain）
```

### 7.2 Runtime Packs

当前 `engine/runtime/packs` 预装配多个 ECS 域。目标不再称 Pack。

按内容裁决：

```text
单一 Scene Feature 的 descriptor/factory
    → engine/scene/features/<name>

一组默认 Feature 的产品配方
    → engine/game/standard_features

Editor-only Feature
    → engine/editor/features
```

示例：

```text
runtime_pack_physics3d
→ engine_scene_feature_physics3d

runtime_pack_presentation3d
→ engine_scene_feature_presentation3d
```

## 8. Frame 与关闭

### 8.1 FrameCoordinator 内收

`FrameCoordinator` 负责：

```text
begin frame
drain main thread mailbox
dispatch Events
run Scene
submit Renderer
safe-point commits
```

这是产品循环的协作实现，不应安装为通用 Runtime Component。

目标：

```text
engine/game/src/GameFrames.*
engine/editor/src/EditorFrames.*
```

可共享一个 `engine/frame/detail/FramePhases` 内部库，但不公开。

### 8.2 统一 CloseTask

将多个公开：

```text
AsyncRuntimeCloseSender
AsyncScopeCloseSender
SceneRuntimeCloseSender
EngineExtensionsCloseSender
RenderEffectCloseSender
```

收敛为 Engine 内部统一协议：

```cpp
class CloseTask
{
public:
    bool poll();
    bool done() const noexcept;
    std::optional<CloseError> error() const;
};
```

底层可以继续用 sender/receiver；产品 API 只暴露一个 close 结果形状。

### 8.3 产品关闭顺序

```text
1. 关闭用户命令与 Extension load admission
2. 停止 Editor/Toolchain producer
3. 关闭 Play/Preview/Game Session
4. 关闭 Scene Features 与 Schedule
5. 关闭 Scene asset/loading tasks
6. 关闭 Extensions
7. 排空 Executor main-thread completion
8. 关闭 AssetStore
9. 关闭 Renderer
10. 关闭 Executor
11. 释放 catalogs、VFS 与平台对象
```

Renderer 与 Executor 的具体先后要以 accepted-work 依赖验证；任何调整必须有故障注入测试。

## 9. CMake 重构

### 9.1 删除 `engine/runtime/CMakeLists.txt` 安装聚合

在所有子目录迁出后 DELETE：

```text
engine/runtime/
lux-engine-runtime package
lux::engine::runtime namespace targets
```

Engine 内部包可按领域安装，或只作为产品内部目标，不必复制 Modules 的公共 SDK 模式。

### 9.2 新内部目标

```text
lux::engine::execution
lux::engine::assets
lux::engine::extensions_api
lux::engine::extensions_loader
lux::engine::scene
lux::engine::scene_package
lux::engine::scene_loading
lux::engine::render_scene_bridge
lux::engine::spatial3d
```

只有 Extension API 需要单独 SDK 安装；其他是否安装取决于外部 Engine Embed use case，不自动进入公共 Modules SDK。

## 10. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| ENG-01 | 物理目录与新 target 骨架，不改实现 | 所有旧 target alias 可用 |
| EXEC-01 | `AsyncRuntime → Executor` 外观 | 异步测试全通过 |
| ASSET-01 | `AssetStore` 建立并迁移 Manager/LoadService | Resource 无 Manager |
| EXT-01 | Extension API/Loader 路径迁移 | Modules SDK 无 Extension |
| EXT-02 | 拆 `EngineExtensions` 与通用 ContributionId | load/unload 测试 |
| SCENE-01 | `SceneRuntime → Scene` 外观 | 当前 Scene tests |
| SCENE-02 | Contribution 表面压缩为 Feature | 事务/回滚测试 |
| SCENE-03 | Entity Loading 重组 | streaming tests |
| RENDER-ENG-01 | BackendHost 移入 Render，保留 Scene Bridge | 外部 Render sample |
| FRAME-01 | Frame/Close 内收产品 | 无公开 runtime_frame |
| ENG-FINAL | 删除 `engine/runtime` 与旧 package | 全仓无旧 namespace |

## 11. 验收闸门

- [ ] 不存在 `engine/module/Modules.hpp` 或等价通用容器。
- [ ] `ExtensionLoader` 不提供服务定位。
- [ ] `Executor` 仍位于 Engine，公共 Modules 不依赖它。
- [ ] `AssetStore` 位于 Engine，公共 Asset 不拥有缓存。
- [ ] `Scene` 不接收通用 Runtime。
- [ ] `Schedule` 仍唯一拥有 System。
- [ ] `RenderBackendHost` 已由公共 `render::Renderer` 取代。
- [ ] `engine/runtime/packs` 已按 Feature 或产品配方归位。
- [ ] `engine/runtime` 目录和安装包最终删除。
- [ ] 多种 CloseSender 不再成为跨领域公共概念。
- [ ] Extension unload 在所有 Lease 与 accepted work 归零后发生。
