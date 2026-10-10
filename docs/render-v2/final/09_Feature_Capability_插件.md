# 09 — Concrete Feature、SceneCapability、不同数据域、可选 Hook 与第三方扩展

## 9.1 没有 Feature 虚基类，没有第二 Feature 框架

一个 Feature 是 concrete C++ 类型加显式 descriptor 和 compile-time `FeatureOps`，可以实现自身实际需要的方法；无方法就无 thunk，不需要 `RenderFeature`/`SceneFeature`/`ViewFeature` 继承层级。与其让所有 Feature 继承 `addPasses()`，不如根据真实职责组合：Resource/Data Provider（Camera, Light, Material, Mesh），compute producer（Skinning/HZB/Cull），graphics effect（Forward/Deferred/Shadow/Postprocess），View state owner（TAA, Camera），Asset streaming consumer（Terrain/PointCloud）。

## 9.2 一套具体接口与 Concepts（C++20）

```cpp
template<class T>
concept HasScenePasses = requires(T& t, SceneGraphContext& ctx, RenderGraphBuilder& graph) {
    { t.declareScenePasses(ctx, graph) } noexcept -> std::same_as<void>;
};

template<class T>
concept HasViewPasses = requires(T& t, const ViewGraphContext& view, RenderGraphBuilder& graph) {
    { t.declareViewPasses(view, graph) } noexcept -> std::same_as<void>;
};

template<class T>
concept HasFrameUpdate = requires(T& t, SceneFrameContext& frame) {
    { t.prepareSceneFrame(frame) } noexcept -> std::same_as<void>;
};

struct FeatureOps {
    void (*declare_scene)(void*, SceneGraphContext&, RenderGraphBuilder&) noexcept{};
    void (*declare_view)(void*, const ViewGraphContext&, RenderGraphBuilder&) noexcept{};
    void (*prepare_scene_frame)(void*, SceneFrameContext&) noexcept{};
    // only essential callback slots; no every-method default virtual thunk
};
```

已知 concrete internal Feature 可 static direct call；跨动态 Feature 注册边界才按 concept 用 `if constexpr` 生成无捕获函数表，并通过 `FeatureInstance`/registration 绑定一次。`FeatureOps` 的函数指针不能逃逸到插件 code pin 之外。可选 hooks 不生成 dummy `return success` thunk。`noexcept` 的前提是业务通过 `RenderResult`/cold configure 返回可恢复错误，不靠抛异常跨线程/插件。

`RenderFeatureContext` 只能暴露受限 typed create/submit/readback/Graph author APIs；不暴露 Backend Device owner、Renderer root、Scene native private resource tables。必要的高级 Vulkan 功能通过独立声明 capability 的受限 extension tier（单独批准），不是私下 `#include VulkanBackend.hpp`。

## 9.3 Descriptor、Capability 与 SceneData 不是同一个东西

`FeatureDescriptor` 声明稳定 FeatureTypeId、版本、实际 Schema/Data sinks、`provides/requires SceneCapability`、可选/硬依赖、scope（runtime/shared/scene/view）、合法设备功能要求。`SceneCapabilityIndex` 是 RenderScene 非拥有冷态投影：每个 Capability 默认最多一个 active authoritative provider；多个 provider 冲突返回 `AMBIGUOUS_SCENE_CAPABILITY`，不靠注册顺序选择 first。对确有多 provider 语义（如多个独立 shadow techniques）须分开 qualified capability key 或有明确合成规则。

消费者 `attach` 时一次 resolve typed pointer/handle 并固定 dependency pin；热路径不再 `find<T>()`、字符串 Hash。Provider 卸载前必须先撤销 capability 对外可见性、阻止新消费者绑定、处理依赖消费者的 detach/rewire，等最后 CPU callback/inflight/native usage 完成再释放 owner。

**例子**：`MeshSceneData` provider 持 InstanceResources；Forward/Deferred/MeshShadow/Highlight 是消费者并贡献 Pass。`LightSceneData` provider 无需 graph pass；DeferredLighting/Shadow 读取。`ViewCameraData` 提供 Camera/Jitter/History epoch；HZB/TAA/Depth 使用但不直接访问 ECS Live Registry。

## 9.4 Feature 装配事务

1. 冷态 `registerFeatureType`：验证 stable type ID/schema version、ABI、FeatureOps、code pin、provided/required capabilities、port routes；生成唯一 runtime-local descriptor/route，不做 eager shader/material GPU work。
2. `prepareAttach`：Feature 构造完整 candidate，解析 required capabilities、资源需求、Shader/pipeline/Graph declared contributions、设备要求；失败释放 candidate，不影响运行中 Scene。
3. `validateComposition`：检查 required/optional capability、View path restrictions、multiple provider conflicts、Graph producer semantics、plugin code lifetime，准备完整新的 RenderPath/Graph Plan/Descriptor candidate。
4. `publish`：在 Backend safe composition point 替换 Feature membership/compilation key；只发布完整 SceneData/Graph，old plan 留到所有 CPU/GPU references 已 safe。
5. `detach`：先 stop receiving typed updates + disable capability + revoke new paths，依赖消费者 detach，old native plan/GPU backing fence-retire，最后销毁 concrete Feature 并解除 DLL code pin。

不需要公共 `RenderFeatureManager`、`FeatureScheduler`、`FeatureStateStore` 与 `FeatureFactoryBase`；生命周期 authority 始终是 RenderScene 的 FeatureInstance set，唯一注册 catalogue 是 Runtime/Backend cold type registry。

## 9.5 数据来源与参数更新的区别

- **Feature 配置**（路径/Shader/格式/Capability 拓扑）：通过 CONTROL typed configuration transaction；可能 native cold Graph/PSO 重新编译；失败保留旧 Feature。
- **运行时数据**（光照值、Camera、颜色/曝光、Instance flags、water surface transform）：通过 PROGRAM typed RenderData 更新已有 persistent Feature/Scene/View state；不触发 Graph compile。
- **大型资产**（Texture/Mesh/Terrain/PointCloud chunks/Shader cooked material）：通过 UPLOAD + receipt + native transfer；CPU source retain 与 GPU in-flight 不同。
- **View 参数变化**（Camera/rect/jitter/output history epoch）：View 的独立 state更新，可在没有 Simulation tick 时生效；必要的 per-frame bindings 不构成新 Graph。
- **实时 Editor 操作**（Selection/Grid/Gizmo/2D overlay）：使用相同 typed transport/Feature APIs，不增加 Editor private Vulkan pass。支持 Feature params reflection+schema typed proxy，Editor 可自动构建控件但不绕开命令合同。

### 具体 Feature 状态模型示例

```text
LightFeature: UpsertLight/RemoveLight PROGRAM → LightSceneData provider → consumers Forward/Deferred/Shadow
SkinningFeature: Bone batch → SkinningResources/Scene pose epoch → Scene-scope Compute → consumed vertex-pool
HZBFeature: View depth + history validity → mip chain Compute per View → cull next frame / resize reset
Canvas2DFeature: Image/Tile/PixelField delta → per-Scene GPU resident arena → View/Target composite
PointCloud variants: chunk upload → shared octree/points; selected render algorithm → cull/indirect/splat/draw
```

每个 Feature 不必拥有自己的 Frame loop 或 GraphCache；GPU data state 由具体 provider/Feature ownership 管理，Graph 表达实际 Render work。

## 9.6 Public SDK 和外部插件的两层接口

**Authoring C++20 层**：公开版本化 FeatureDescriptor、typed RenderData schema、PassParams generated traits、GraphBuilder、Scene/View/Frame restricted contexts、cooked Shader asset interface。外部插件应能创建 graphics/compute/transfer Pass、共享 SceneCapabilities、处理自己拥有的 GPU 资源和 Query/readback，但不能包含 private Vulkan native `pinclude/sinclude`。

**跨动态库 ABI 边界**：使用显式 `extern "C"` 版本化注册入口、`struct_size/abi_version/feature_canonical_id`、稳定布局 POD/opaque handles/function tables、host allocator/error/retain/lease conventions；插件 C++20 模板仅在自己的编译单元里使用。跨插件边界默认不传 `std::vector`, C++ exceptions, owning `std::unique_ptr`/`shared_ptr` 或依赖某一 CRT 的隐含所有权（除非完全相同编译器/CRT 的受控 ABI 经单独验证）。插件 callbacks 到 Host 的同步/错误/线程规则在注册时固定，不能 runtime RTTI/ServiceLocator。

`FeatureOps` 负责受限能力请求/Pass record，binary/memory ownership 由 host 的 opaque typed IDs 管理。高级原生扩展必须遵循清楚的 capability isolation 与危害可审计 boundary，不等价于 `VkDevice*` 外泄。

## 9.7 Code pin / plugin unload 的完整证明

```text
plugin register → FeatureInstance active + code pin
   → Graph compilation stores callback/Shader/Pipeline needed pin
   → Frame/Backend recorder borrows only while pin held
   → user requests detach/unload
   → revoke new work and SceneCapability attachment
   → wait for CPU callback quiescence / stop new Graph use
   → GPU submissions complete and native plan/resource retirement
   → destroy Feature owned state and record programs
   → release last code pin
   → plugin DLL unload permitted
```

注意 GPU 不执行 CPU callback，但 native plan/compiled callbacks may hold code/owning objects until CPU retirement. 不能因为一次帧结束就卸载 callback，不能通过无限 `shared_ptr` 循环把 DLL 永久 pin，也不能让 Feature 析构访问已销毁 Device/VMA。

## 9.8 验收必须是真独立消费者

- Native library under installed prefix + independently compiled external plugin (outside repo source)；正向 create/attach/PassParams/shader load/record/readback output。
- 负例：缺 required capability、两个 competing provider、ABI/schema version mismatch、非法 unsupported GPU capability、外部 include private header、过期 handles/code pin/unload while GPU in flight。
- plugin unload/reload 不得破坏另一个仍 active Feature；跨 DLL 真实 host authority 不重复静态 registry/local TypeId。
- 输入未变时 Provider 不频繁发包/无 empty Pass；多 View/多 Target 与 built-ins 共图执行。

Legacy 外部样例只作为迁移 provenance；不能直接复制其 `void* scene + RenderFeature abstract base` 设计作为 V2 合格 SDK。
