# Function 公共模块重构

> **2026-08-21 依赖裁决：** Function Animation 只消费 Description 中的 Skeleton/AnimationClip 值与 Core Math，不应通过 Resource Asset 获得这些类型。本轮将删除该陈旧 PUBLIC Asset 依赖，不改变采样 API 或 wire。

> 纯化 Render、Input、Animation、Navigation、Script 与 UI 的公共闭包，使其真正可脱离 ECS 和 Engine 使用

**执行文档 04 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Render、Input、Animation、Navigation、Script、UI 与公共 SDK 负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 关联裁决：** Function 仍只依赖通用 Resource 契约；本文出现的 “AssetStore” 应理解为现有公共 `AssetManager` 的上层使用者，不再要求创建 Engine AssetStore。详见 `ADR-20260820_SceneAsset与Resource边界.md`。


## 1. 目标与边界

`modules/function` 仍是公共 SDK 的领域能力层，但不再作为“所有非 ECS 代码”的泛化容器。每个模块必须有清晰外部产品价值和最小依赖闭包。

| 当前目标 | 目标公开目标 | 核心处理 |
| --- | --- | --- |
| `render_client` | `lux::render` 或内部 protocol 子目标 | 移除 Meta、Deployment、Platform Common 的 PUBLIC 依赖 |
| `render_graph` | `lux::render_graph` | 保持设备无关；依赖 core/math/containers，不依赖 Platform Common |
| `render_vulkan` | `lux::render_vulkan` | 消费保留的 GAPI；Window Surface 作为叶子 Integration |
| `render_features` | `lux::render_standard` + 私有 feature object libs | Grid/Gizmo/Highlight 工具能力上移或可选 |
| `input` | `lux::input` | 物理输入值不依赖 Window Backend |
| `animation` | `lux::animation` | 只依赖 Description 和 Math |
| `navigation` | `lux::navigation` | 依赖 Math，不依赖 resource/spatial |
| `script_core` | `lux::script` | ABI、Signature、Value 与非空 CallFrame；无通用 Runtime dispatcher |
| `script_lua` | `lux::script_lua` | LuaJIT/sol2 具体执行能力；会话绑定归 ECS backend |
| `script_native` | `lux::script_native` | move-only NativeModule + DynamicLibrary；与 Engine Extension ABI 无关 |
| `ui` | `lux::ui` + `lux::ui_imgui` | Panel/Widget 与 ImGui Backend 分离 |
| `ui_vulkan` | `lux::ui_render_vulkan` | 只做 UI draw data 到 Render/Vulkan Integration |

## 2. Render：公共 SDK 的主边界

### 2.1 目标分层

```text
modules/function/render/
├── api/             backend-neutral Renderer、Frame、View、Resource Handles
├── graph/           logical render graph
├── shader/          shader/layout contract
├── vulkan/          Vulkan backend 与内部 low-level wrappers
├── standard/        可选标准渲染能力集合
└── integrations/
    ├── window_vulkan/
    └── ui_vulkan/
```

公开目标：

```text
lux::render
lux::render_graph
lux::render_vulkan
lux::render_standard
```

内部目标可以更多，但不进入安装组件列表。

### 2.2 `render_client` 的处理

当前 `render_client` 包含大量 Operation Client、Frame/Control/Upload Session 与生成通信代理。保留其线程隔离和 Channel 设计，但对外表面重组为 Renderer API。

目标：

```cpp
namespace lux::render
{
    class Renderer;

    class Frame final
    {
    public:
        SceneView createView(...);
        void submit(RenderPacket);
        expected<void, FrameError> finish();
    };

    class UploadQueue final
    {
    public:
        UploadTicket upload(BufferUpload);
        UploadTicket upload(ImageUpload);
    };
}
```

`RenderControlSession`、`RenderFrameSession`、`RenderUploadSession` 可作为内部实现类型；普通外部用户不应同时理解三套 Session 和每个 Operation Client。

### 2.3 删除 PUBLIC Meta 依赖

当前 `render_client` PUBLIC 链接 `core::meta`，主要用于生成通信 Operation。施工：

1. 生成器在 Build-time 读取声明；
2. 生成 `.ops.hpp/.cpp`；
3. Runtime 目标只编译生成结果；
4. 安装 Config 不查找 Meta Generator；
5. 若生成结果需要 `TypeId`，使用轻量 `lux-cxx::type_hash` 或显式 Operation ID。

禁止：

```cmake
target_link_libraries(render_client PUBLIC meta)
```

### 2.4 删除 Deployment 依赖

`RuntimeCapacity` 拆分：

- GPU/Renderer Capacity 类型迁入 `render/config`；
- Game Product Capacity Request 留在 Engine Game Manifest；
- Renderer 构造接收 `RenderConfig`，不读取 Game Manifest。

目标：

```cpp
struct RenderCapacity final
{
    std::uint64_t maximumInstances{};
    std::uint64_t geometryBytes{};
    std::uint32_t bindlessTextures{};
};

struct RenderConfig final
{
    RenderCapacity requested{};
    Validation validation{Validation::enabled};
};
```

### 2.5 `render_graph`

当前目标只编译 `DependencyAnalyzer.cpp`，保持设备无关。修改：

```cmake
target_link_libraries(render_graph
    PUBLIC
        lux::cxx::container
        lux::cxx::compile_time
)
```

移除 `platform::common`。`Size2D`、Format 等精确依赖改为 `lux::math` 或 `lux-cxx`。

### 2.6 `render_vulkan` 消费保留的 GAPI

按 ADR-20260821，GAPI 继续作为公共 Platform Vulkan wrapper SDK。`render_vulkan` 可以链接并使用它，但不迁移其目录、target、component 或 namespace，也不在本重构中强制把 GAPI wrapper 与 Render 内部 handle 合并为一种公共对象模型。

### 2.7 Window Surface Integration

`render_vulkan` 核心只接收抽象 Surface Factory 或已创建 Surface：

```cpp
struct SurfaceSource
{
    platform::NativeWindowHandle window;
};

expected<Surface, SurfaceError>
createVulkanSurface(VulkanInstance&, SurfaceSource);
```

目标依赖：

```text
render_vulkan core       不依赖 window_glfw
render_vulkan_window     依赖 window + render_vulkan
```

### 2.8 `render_features` 收敛

当前一个目标包含 Deferred、Forward、Grid、Gizmo、Highlight、Point Cloud、Terrain、Water、Shadow、Skybox、Postprocess 等大量实现。

施工分组：

| 分组 | 内容 | 安装策略 |
| --- | --- | --- |
| `standard_core` | View、Depth、Forward/Deferred 基础、Tonemap | 合入 `lux::render_standard` |
| `standard_lighting` | Light、Shadow、BRDF、Skybox | 合入 `lux::render_standard` |
| `standard_geometry` | Mesh、Skinning、Material | 合入 `lux::render_standard` |
| `render_tooling` | Grid、Gizmo、Highlight、Picking Overlay | 默认不装；Editor/Tooling 使用 |
| `render_pointcloud` | Point Cloud Feature | 确有独立用户时公开可选 |
| `render_terrain` | Terrain/Water | 确有独立用户时公开可选 |
| `render_streaming` | Feedback、LOD、Cull | 可作为 standard 内部 |

先使用 Object Library 拆编译单元，再决定公开组件，避免组件爆炸。

### 2.9 Renderer 主对象

公共 API：

```cpp
namespace lux::render
{
    class Renderer final
    {
    public:
        static expected<Renderer, OpenError>
        open(RenderConfig, Backend, Surface);

        Frame beginFrame(FrameInfo);
        UploadQueue& uploads() noexcept;
        SceneView createSceneView(SceneViewConfig);
        OffscreenView createOffscreenView(OffscreenConfig);

        CloseTask close();

        Renderer(Renderer&&) noexcept;
        Renderer& operator=(Renderer&&) noexcept;
        ~Renderer();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
```

公共 `Renderer.hpp` 不包含 Vulkan 类型；`Backend` 可由 Vulkan 工厂产生。

## 3. Input 解耦

### 3.1 当前问题

历史基线中 `input` 的 Action Mapping PUBLIC 依赖 `platform::window`，并向消费者泄漏 GLFW。`08e3d590` 已完成归位：Input 的公开链接闭包只保留 `lux-cxx::container`，Window/GLFW 仅为平台采样实现的 PRIVATE 依赖。

### 3.2 已实施结构

```text
modules/function/input/
├── include/lux/engine/input/
│   ├── Input.hpp
│   ├── InputSnapshot.hpp
│   ├── PhysicalInput.hpp
│   └── Action/Mapping/Context headers
└── src/
    ├── Input.cpp
    ├── ActionMapper.cpp
    ├── BindingIdAllocator.cpp
    └── platform/{glfw|android}/InputPlatform.cpp
```

目标值类型：

```cpp
enum class EKey;
enum class EMouseButton;
struct InputSnapshot;
```

公开领域对象：

```cpp
class Input final
{
public:
    void sample(window::LuxWindow&);
    void evaluate(float dt, bool accept_keyboard, bool accept_pointer);
    ActionMapper& mapper() noexcept;
    InputActionRegistry& actionRegistry() noexcept;
    InputContextStack& contexts() noexcept;
};
```

Input 只有一个 target。CMake 在配置期只选择一个私有平台实现；不创建 Adapter target、backend interface、factory 或注册表。`lux::input` 公共头不 include `<GLFW/glfw3.h>`。

Window 已删除 normalized input 与 snapshot 状态，只积累 raw key/mouse/scroll/text event batch。`Input::sample()` 负责 held/edge、cursor/sample history 与规范化；`evaluate(InputSnapshot, ...)` 可在无 Window 的 installed consumer 中运行。

### 3.3 对外对象命名

`Input` 是平台宿主使用的统一所有权对象：

```cpp
class Input final
{
public:
    void sample(window::LuxWindow&);
    void evaluate(float dt, bool accept_keyboard = true,
                  bool accept_pointer = true);
    ActionMapper& mapper() noexcept;
    InputActionRegistry& actionRegistry() noexcept;
    InputContextStack& contexts() noexcept;
};
```

`InputActionRegistry` 只保留 `ActionMapper` 内部唯一实例。`ActionMapper` 仍可在 headless Preview 与低层 Runtime 中单独使用，但 GameApplication 不得再维护第二份 Registry。

## 4. Animation 解耦

历史基线中 `animation` PUBLIC 依赖 `asset_core`；`d1ead288` 已改为直接依赖
Description 与 Math。目标函数直接操作 Description：

```cpp
Pose sample(
    const description::Skeleton&,
    const description::AnimationClip&,
    AnimationTime,
    Scratch&);
```

ECS `AnimationSystem` 与现有 Engine Runtime 资产编排负责：

```text
AssetHandle → Skeleton/AnimationClip value → lux::animation::sample()
```

MODIFY：

```text
modules/function/animation/CMakeLists.txt
```

移除：

```text
lux::engine::resource::asset
```

新增：

```text
lux::engine::resource::description
lux::engine::core::math
```

## 5. Navigation 解耦

当前 `navigation` 依赖 `resource::spatial`。Spatial 值迁入 Math 后：

```cmake
target_link_libraries(navigation
    INTERFACE
        lux::math)
```

Detour3D Backend 保持独立：

```text
lux::navigation
lux::navigation_detour3d
```

ECS Navigation Region、Agent Component 与 System 留在 `ecs/navigation`。

## 6. Script ABI、具体 Module 与直接调用

### 6.1 删除通用 ScriptRuntime dispatcher

`ScriptRuntime`、`ScriptFunctionHandle`、`ScriptFunction`、`IScriptModule` 把冷路径
加载与热路径调用错误地捆绑在一起。按
`ADR-20260821_ScriptAsset会话驻留与直接调用.md`，这一通用 dispatcher 删除，不保留
handle、alias 或 forwarding header。

`script_core` 只拥有 Script ABI、value/signature view、非空 `CallFrame` 便利层和加载/
绑定阶段的结构化错误。`script_native` 公开一个具体 move-only `NativeModule`，
它拥有 `DynamicLibrary`并在绑定期暴露 `lux_script_function_desc`。

### 6.2 调用是绑定后的函数指针

ECS/Engine 绑定 ScriptEvent 时完成函数名解析、ABI 校验和精确签名匹配，并保存
`{lux_script_invoke_fn, void* context}`。调用期间不再进入 Function 层的 Runtime、虚接口、
锁、hash/string lookup、`shared_ptr` 或 `expected`。

Lua/Native backend 的多态只存在播放会话绑定冷路径；不得进入每实例每帧调用。

### 6.3 Native Script 与 Engine Extension 分开

```text
script_native
    加载脚本编译产物
    使用 Script ABI
    依赖 DynamicLibrary

engine/extensions
    加载 Lux Engine Extension
    使用 Extension ABI
    注册 ECS/Scene/Render/Execution 能力
```

任何头文件不得同时 include 两种 ABI。

## 7. UI 边界（待独立 ADR 重新裁决）

> 以下四 target 结构是历史候选，不再作为可直接施工的固定答案。后续必须先核对 ImGui、Window、
> Vulkan 与 Editor Viewport 的真实所有权；不得通过创建薄 `ui_imgui_glfw` Adapter 来掩盖边界。
> `SceneViewportPanel` 迁入 Editor、公共 UI 闭包退出不必要的平台/渲染依赖仍是有效目标。

### 7.1 当前问题

基础 `ui` 目标 PUBLIC 链接 ImGui GLFW 与 Vulkan，并包含 `SceneViewportPanel`；这使“使用 Panel”自动获得窗口与 Vulkan 依赖。

### 7.2 目标结构

```text
modules/function/ui/
├── core/
│   ├── Panel.hpp
│   ├── Widget.hpp
│   ├── Layout.hpp
│   └── Command.hpp
├── imgui/
│   ├── ImGuiContext.hpp
│   └── ImGuiWidgets.cpp
├── integrations/
│   ├── imgui_glfw/
│   └── imgui_render_vulkan/
└── CMakeLists.txt

engine/editor/
└── viewport/
    └── SceneViewport.cpp
```

历史候选目标（非现行强制 target 清单）：

```text
lux::ui
lux::ui_imgui
lux::ui_imgui_glfw
lux::ui_render_vulkan
```

### 7.3 `UISystem`

若它拥有 ImGui Context 与每帧绘制，RENAME：

```text
UISystem → UI
```

因为 `System` 只保留给 ECS。

### 7.4 `SceneViewportPanel`

MOVE：

```text
modules/function/ui/src/SceneViewportPanel.cpp
modules/function/ui/include/.../SceneViewportPanel.hpp
→ engine/editor/src/viewport/SceneViewport.cpp
```

它知道 Lux Scene/Render View/Editor selection，不是通用 UI。

### 7.5 Backend 依赖

```cmake
target_link_libraries(ui PUBLIC /* no GLFW, no Vulkan */)
target_link_libraries(ui_imgui PUBLIC imgui::core)
target_link_libraries(ui_imgui_glfw PRIVATE glfw)
target_link_libraries(ui_render_vulkan PRIVATE lux::render_vulkan)
```

## 8. Shader 与 Codegen

Shader Layout、Pass Params、通信 Operation Codegen 都属于 Render Build Tooling。建立：

```text
modules/function/render/shader/
cmake/Codegen/RenderOperations.cmake
cmake/Codegen/ShaderParams.cmake
```

生成器可由 Toolchain Profile 构建，但生成产物目标不 PUBLIC 链接生成器。

`render_features` 中对 `include_component_cmake_scripts(meta)` 的依赖改为通用 Codegen CMake API，避免公共 Render 包需要 Engine Meta Component。

## 9. CMake 修改总表

| 当前文件 | 修改 |
| --- | --- |
| `modules/function/CMakeLists.txt` | 显式子目录；安装包按领域拆分 |
| `render/client/CMakeLists.txt` | 删除 Meta、Deployment、Platform Common 的 PUBLIC 依赖 |
| `render/graph/CMakeLists.txt` | 精确依赖 Core/Math/Container |
| `render/vulkan/CMakeLists.txt` | 消费保留的 GAPI；Surface Integration 分离 |
| `render/features/CMakeLists.txt` | Object Library 分组；工具 Feature 移出默认标准包 |
| `input/CMakeLists.txt` | 不 PUBLIC 链接 Window |
| `animation/CMakeLists.txt` | 依赖 Description，不依赖 Asset Core |
| `navigation/core/CMakeLists.txt` | 依赖 Math |
| `script/core/CMakeLists.txt` | ABI/Signature/Value/CallFrame 与加载期 expected 错误；无 Runtime handle |
| `ui/CMakeLists.txt` | Core/ImGui/GLFW/Vulkan/Editor Viewport 拆分 |

## 10. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| FUNC-01 | 新公共 Alias 与 Include Prefix | 旧 API 兼容 |
| RENDER-01 | Render Config 与 Deployment 解耦 | render_client 无 deployment |
| RENDER-02 | Build-time Meta 依赖移出 Runtime Link | 安装包无 generator |
| RENDER-03 | GAPI 保留裁决 | 迁移/删除目标 SUPERSEDED，公共 GAPI 保持可链接 |
| RENDER-04 | Window Surface Integration 分离 | render_vulkan core 无 GLFW |
| RENDER-05 | features Object Library 分组 | 行为与 shader 输出不变 |
| INPUT-01 | Snapshot 与 Window Backend 分离 | input-only 样例无 GLFW |
| ANIM-01 | Animation 直接依赖 Description | 无 AssetStore |
| NAV-01 | Navigation 改依赖 Math | resource/spatial 删除 |
| SCRIPT-01 | NativeModule 加载期 expected + ECS 直接 ABI 分派 | Native/Lua/session/机器码测试通过 |
| UI-01 | UI owner ADR 与实现收敛 | UI 公共闭包无不必要的 GLFW/Vulkan，且不制造薄 Adapter target |
| UI-02 | SceneViewport 迁入 Editor | modules/ui 无 Scene 类型 |
| FUNC-FINAL | 删除旧 targets/include | Modules SDK 闭包纯净 |

## 11. 验收闸门

- [ ] `lux::render` 公共头无 ECS、Scene、Editor 类型。
- [ ] `lux::render` 安装 Config 无 Meta Generator、Deployment、Extension ABI。
- [ ] `lux::render_vulkan` 核心不依赖 GLFW。
- [x] `platform/gapi` 按保留 ADR 继续作为公共组件存在。
- [ ] `render_graph` 可在无 Vulkan SDK 的测试配置中编译。
- [ ] `lux::input` 可在无 GLFW 的配置中编译。
- [ ] `lux::animation` 不依赖 Asset Core/AssetStore。
- [ ] `lux::navigation` 不依赖 Resource Spatial。
- [ ] Script ABI 与 Extension ABI 无共享头。
- [ ] `lux::ui` 无 GLFW/Vulkan 依赖。
- [ ] `SceneViewportPanel` 不在 modules。
- [ ] Tooling Feature 不自动进入标准 Renderer 公共闭包。
