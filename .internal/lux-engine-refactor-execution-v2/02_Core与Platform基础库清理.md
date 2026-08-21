# Core 与 Platform 基础库清理

> 移除 Engine、ECS 与 Vulkan 语义对基础层的污染，并把 common/meta/extension_abi 拆回真实所有者

**执行文档 02 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Core、Platform、Reflection、Serialization、ECS 与 Render 负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 关联裁决：** 本文 Core/Platform 目标不变。资产身份、Scene Asset 与场景 Payload 的现行边界见 `ADR-20260820_SceneAsset与Resource边界.md`；不得为该迁移把 Engine Scene 语义下沉到 Core。

> **2026-08-21 裁决更新：** Core Meta 删除 EnTT、Registry、`LuxObject` 与 `EntityObject`；Registry allocator/handle/publication 合同原样归 `ecs/core`。反射类型由标注 record identity 决定，不再依赖 OO 根类。详见 `ADR-20260821_CoreMeta纯化与ECSRegistry归位.md`。

> **2026-08-21 实施状态：** 上述 Meta/Registry 裁决已完成。Core Meta/Serialization installed consumer 不导入 EnTT；ECS-owned adapter 承担 EnTT component 操作，旧 Meta Registry 与 OO 根类已归零。


## 1. 施工范围

本文件处理以下目录：

```text
modules/core/events
modules/core/log
modules/core/math
modules/core/meta
modules/core/serialization
modules/core/extension_abi

modules/platform/common
modules/platform/dynamic_library
modules/platform/filewatch
modules/platform/gapi
modules/platform/window
```

目标不是把所有东西都压入 `core`。Core 只容纳真正通用、与 Engine/ECS 无关的基础能力；Platform 只容纳操作系统与平台边界。

## 2. 文件迁移总表

| 当前文件/目录 | 目标 | 动作 |
| --- | --- | --- |
| `modules/core/extension_abi/include/.../StableId.hpp` | `lux-cxx::StableNameId` remains generic; domain IDs move to owners | SPLIT |
| `modules/core/extension_abi/include/.../ModuleAbi.hpp` | `engine/extensions/api/include/lux/engine/extensions/ExtensionAbi.hpp` | MOVE |
| `modules/core/meta/include/lux/engine/meta/LuxObject.hpp` | `ecs/core/include/lux/ecs/EntityRegistry.hpp` and optional `EntityObject.hpp` | SPLIT |
| `modules/core/meta/include/lux/engine/meta/Meta*.hpp` | `lux-cxx::reflection_runtime` or temporary `engine/reflection` | SPLIT |
| `modules/core/meta/cmake/engine_add_meta.cmake` | `cmake/Codegen/Reflection.cmake` | MOVE |
| `modules/core/serialization/src/TaggedPropertyArchive.cpp` | entire reflected Component Archive to `ecs/serialization`; Core retains Archive/NameTable | MOVE/DELETE |
| `modules/resource/spatial/include/.../Spatial.hpp` | `modules/core/math/include/lux/math/Position.hpp` and `Grid.hpp` | MOVE |
| `modules/platform/common/include/.../AtomicWait.hpp` | `lux-cxx::concurrent` or `modules/core/concurrency` | MOVE |
| `modules/platform/common/FormatCompat.h.in` | `lux-cxx::format` or `modules/core/format` | MOVE |
| `modules/platform/common/include/.../Size2D.hpp` | `modules/core/math/include/lux/math/Extent.hpp` | MOVE |
| `modules/platform/common/include/.../ImageEnums.hpp` | `modules/resource/description/include/lux/description/Image.hpp` | MOVE |
| `modules/platform/gapi` | `modules/function/render/vulkan/low_level` | MOVE |
| `modules/platform/window/src/GlfwRuntime.cpp` | `modules/platform/window/glfw/src/GlfwLibrary.cpp` | MOVE/RENAME |
| `modules/platform/window/src/TrayIconWin32.cpp` | `modules/platform/tray/win32` or product layer | MOVE |

## 3. `extension_abi` 迁出 Core

### 3.1 当前错误

`ModuleAbi.hpp` 当前直接定义：

```text
Lux Engine Extension ABI v4
Runtime / Editor Extension Target
RuntimeContributionRegistrar
EditorContributionRegistrar
动态库导出符号
```

这不是 Core ABI，而是 Lux Engine Extension SDK。

### 3.2 目标目录

CREATE（2026-08-21 ADR 修订后）：

```text
engine/extensions/api/
├── CMakeLists.txt
├── include/lux/engine/extensions/
│   ├── ExtensionId.hpp
│   ├── ExtensionVersion.hpp
│   ├── ExtensionDescriptor.hpp
│   ├── ExtensionAbi.hpp
│   ├── ExtensionResult.hpp
│   └── ExtensionRegistrarFwd.hpp
└── test/
    └── extension_abi_layout_test.cpp
```

MOVE：

```text
ModuleAbi.hpp → ExtensionAbi.hpp
ExtensionIdTag/ExtensionId → ExtensionId.hpp
ExtensionDependencyView → ExtensionDependencyView
ExtensionModuleDescriptorV4 → ExtensionDescriptorV4
```

RENAME：

```text
kExtensionAbiV4                 → kExtensionAbiVersion4
EExtensionModuleTarget          → ExtensionTarget
ExtensionRegistrationResult     → ExtensionResult
EExtensionRegistrationError     → ExtensionError
GetExtensionModuleV4Fn          → GetExtensionDescriptorV4Fn
```

ABI 导出符号可以在兼容期保持旧字符串；C++ 类型名和路径先迁移，ABI v5 再决定是否改符号。

### 3.3 Stable ID 解耦

DELETE `StableId.hpp` 中对 `ExtensionId` 与 `ContributionId` 的混合定义。

通用基础只保留已有：

```cpp
lux::cxx::StableNameId<Tag>
lux::cxx::StableNameIdView<Tag>
```

各领域自己定义：

```cpp
namespace lux::ecs {
struct ComponentSchemaIdTag;
using ComponentSchemaId = lux::cxx::StableNameId<ComponentSchemaIdTag>;
}

namespace lux::render {
struct FeatureIdTag;
using FeatureId = lux::cxx::StableNameId<FeatureIdTag>;
}

namespace lux::engine::scene {
struct FeatureIdTag;
using FeatureId = lux::cxx::StableNameId<FeatureIdTag>;
}

namespace lux::engine::extensions {
struct ExtensionIdTag;
using ExtensionId = lux::cxx::StableNameId<ExtensionIdTag>;
}
```

禁止重新引入跨领域 `ContributionId`。

## 4. `meta` 拆分

### 4.1 当前职责

当前 `modules/core/meta` 同时包含：

```text
通用 RefType/RefClass/RefField
ReflectionRegistry
代码生成 CMake
EntityRegistry / EntityHandle / EntityObject
动态库加载后的 pending sidecar drain
RegistryMemoryResource
```

这是至少三个领域。

### 4.2 拆分目标

#### A. 通用 Reflection Runtime

优先合入 `lux-cxx::reflection_runtime`：

```text
RefType
RefClass
RefField
RefMethod
AnnotationView
ReflectionDraft
ReflectionRegistry 的纯注册/查询能力
```

若短期无法合入 sibling repository，建立临时：

```text
modules/core/reflection_runtime/
```

但其验收条件是：

- 不 include EnTT；
- 不定义 Entity；
- 不知道 Extension；
- 不知道 Scene；
- 不调用 DynamicLibrary；
- 不包含 Engine 产品符号。

#### B. ECS Registry

MOVE 到 `ecs/core`：

```text
EntityRegistryBase
EntityRegistry
EntityHandle
ConstEntityHandle
RegistryMemoryResource
EntityObject（若仍需要）
```

目标接口：

```cpp
namespace lux::ecs
{
    using Entity = entt::entity;
    inline constexpr Entity nullEntity = entt::null;

    class Registry final : public RegistryBase
    {
    public:
        Registry();
        explicit Registry(RegistryMemoryUpstream& upstream);

        Registry(const Registry&) = delete;
        Registry& operator=(const Registry&) = delete;
    };
}
```

`LuxObject` 若只为统一虚析构存在，应删除。若生成系统确实需要标记，使用无状态 Concept/trait：

```cpp
template<class T>
concept ReflectedObject = requires { typename T::lux_reflected_tag; };
```

#### C. Engine Reflection Publication

动态 Extension 加载后的 Draft Commit、Sidecar Lease 与 unload protection 放入：

```text
engine/extensions/reflection/
```

不能让基础 Reflection Registry 知道 `ExtensionLoader`。

### 4.3 生成器 CMake

MOVE：

```text
modules/core/meta/cmake/engine_add_meta.cmake
→ cmake/Codegen/Reflection.cmake
```

Build Tool 依赖使用：

```cmake
add_dependencies(target generated_meta)
target_sources(target PRIVATE ${generated_sources})
```

禁止通过：

```cmake
target_link_libraries(target PUBLIC meta_generator)
```

把生成器带入安装闭包。

## 5. Serialization 拆分

### 5.1 保留在公共 Core

```text
ArchiveReader
ArchiveWriter
NameTable
ByteCursor
BoundsCheckedReader
基础 Tagged Record Wire Format
UUID 编解码
```

目标公开依赖不应包含 EnTT 或 Engine Reflection。

### 5.2 上移的 Adapter

CREATE：

```text
ecs/serialization/
    ComponentArchive.hpp/.cpp

engine/serialization/
    ExtensionSchemaMigration.hpp/.cpp
```

`TaggedPropertyArchive` 依赖 `RefClass/RefField`，整体归入 ECS Component Archive；Core 不再保留 tagged wire facade：

```cpp
// ecs/serialization
ComponentArchiveResult<void>
TaggedPropertyWriter::writeObject(const RefClass&, const void*);
```

### 5.3 CMake 修改

MODIFY `modules/core/serialization/CMakeLists.txt`：

```cmake
target_link_libraries(serialization
    PUBLIC
        lux::cxx::binary
        stduuid
)
```

移除 PUBLIC：

```text
lux::engine::core::meta
Eigen3::Eigen（若仅个别 Adapter 使用）
```

Eigen leaf Codec 是 ECS Component Archive 的 PRIVATE 实现依赖。

`RegistryArchive` 提案被 `EntitySectionImage -> EntityBatchStager -> Registry` 边界取代；
Core/ECS 均不建立 `entt::registry` 文件镜像。

## 6. Math 吸收 Spatial 值

### 6.1 文件拆分

MOVE：

```text
modules/resource/spatial/include/.../Spatial.hpp
→ modules/core/math/include/lux/math/Position.hpp
→ modules/core/math/include/lux/math/Grid.hpp
→ modules/core/math/include/lux/math/RelativePosition.hpp
```

目标：

```cpp
namespace lux::math
{
    struct Position2d final { double x{}; double y{}; };
    struct Position3d final { double x{}; double y{}; double z{}; };

    struct GridCoord2i64 final { std::int64_t x{}; std::int64_t y{}; };
    struct GridCoord3i64 final { std::int64_t x{}; std::int64_t y{}; std::int64_t z{}; };

    [[nodiscard]] std::optional<std::array<float, 3>>
    relative(const Position3d&, const Position3d&, float maximumExtent) noexcept;
}
```

不要在值类型头中包含：

```text
MetaAnnotations.hpp
MetaDef.hpp
ReflectionRegistry
```

反射声明由 ECS sidecar 或 Codegen 输入清单维护。

### 6.2 迁移调用点

全仓替换：

```text
lux::spatial::Position2D → lux::math::Position2d
lux::spatial::Position3D → lux::math::Position3d
lux::spatial::GridCoord2i64 → lux::math::GridCoord2i64
lux::spatial::GridCoord3i64 → lux::math::GridCoord3i64
```

旧 namespace 可以保留一个版本的 alias header，但新代码禁止使用。

## 7. 删除 `platform/common`

### 7.1 逐文件归属

| 文件 | 目标 |
| --- | --- |
| `AtomicWait.hpp` | 优先贡献到 `lux-cxx::concurrent`；临时可放 `modules/core/concurrency` |
| `FormatCompat.h.in` | 优先贡献到 `lux-cxx::format`；临时 `modules/core/format` |
| `Size2D.hpp` | `lux/math/Extent.hpp` |
| `ImageEnums.hpp` | `lux/description/Image.hpp` |

DELETE：

```text
modules/platform/common/CMakeLists.txt
lux::engine::platform::common
lux-engine-platform common component
```

所有依赖者必须改为精确目标，不能创建新的 `foundation_common`。

## 8. `gapi` 迁入 Render Vulkan

### 8.1 当前事实

`modules/platform/gapi` 强制查找 Vulkan，并导出 Buffer、Image、Descriptor、Pipeline、Swapchain、Surface 等 Vulkan Wrapper。它是 Render Backend 的底层实现，不是 Platform。

### 8.2 目标目录

MOVE：

```text
modules/platform/gapi/include/lux/engine/gapi/vk/*
→ modules/function/render/vulkan/low_level/include/lux/render/vulkan/*
```

MOVE 或 DELETE：

```text
RenderDevice.hpp
RenderInstance.hpp
```

若只是旧抽象壳，合入 `VulkanDevice` 与 `VulkanInstance`。

MODIFY `render_vulkan`：

```cmake
target_sources(render_vulkan PRIVATE
    low_level/src/...
)
```

DELETE target：

```text
lux::engine::platform::gapi
```

Renderer 对外 API 仍不得暴露这些 Wrapper；它们只用于高级用户明确选择 `lux::render_vulkan_low_level` 时才考虑安装。默认全部 PRIVATE。

## 9. Window 拆分

### 9.1 目标目标

```text
lux::window          平台中立 Window、事件、NativeHandle
lux::window_glfw     GLFW Backend
lux::window_android  Android Backend
lux::render_vulkan_window  Window ↔ Vulkan Surface Integration
```

### 9.2 公共 Window API

```cpp
namespace lux::platform
{
    struct WindowExtent { std::uint32_t width{}; std::uint32_t height{}; };

    struct NativeWindowHandle
    {
        void* value{};
        NativeWindowKind kind{};
    };

    class Window
    {
    public:
        virtual ~Window() = default;
        virtual void pollEvents() = 0;
        virtual bool closeRequested() const noexcept = 0;
        virtual WindowExtent extent() const noexcept = 0;
        virtual NativeWindowHandle nativeHandle() const noexcept = 0;
    };
}
```

Window Core 不 include Vulkan。Vulkan Surface 创建位于：

```cpp
render::vulkan::createSurface(
    VulkanInstance&,
    platform::NativeWindowHandle);
```

### 9.3 GLFW 依赖

当前 `window` 对外 PUBLIC 链接 GLFW，理由是测试和工具直接调用 `glfwGetKey`。目标是所有输入通过 `InputSnapshot`，因此：

```cmake
target_link_libraries(window_glfw PRIVATE glfw)
```

外部消费者不再自动获得 GLFW 头和 ABI。

### 9.4 Tray Icon

`TrayIconWin32.cpp` 不属于 Window Core。若仅 Launcher 使用，迁入：

```text
products/launcher/platform/win32
```

若确认是通用 SDK 能力，建立独立 `lux::tray`，不得留在 Window。

## 10. 保留模块的清理

### 10.1 Events

RENAME 可分阶段进行：

```text
DomainEvents → Events
EventPump → Dispatcher 或内部 Queue
```

公开文档不再说“Host-owned Pump”；改为“owner-thread dispatch point”。

### 10.2 Log

Engine 的异步转发器留在 `engine/logging`。公共 Log 只提供：

```cpp
Logger
Sink
LogRecord
Category
Level
```

### 10.3 Dynamic Library 与 Filewatch

保持现有 RAII 和后端隐藏；仅迁移 include prefix 与 target alias。

## 11. Pull Request 序列

| PR | 施工内容 | 退出闸门 |
| --- | --- | --- |
| CORE-01 | 新建新路径与 compatibility headers | 无行为变化 |
| CORE-02 | Spatial 值迁入 Math | ECS/Navigation/Render 全部通过 |
| CORE-03 | Serialization 基础与反射 Adapter 分离 | 公共 Serialization 无 EnTT |
| CORE-04 | EntityRegistry 迁入 ECS | Meta 公共目标不再链接 EnTT |
| CORE-05 | Extension ABI 迁入 Engine SDK | Modules SDK 无 Extension 类型 |
| PLATFORM-01 | 解散 common | 所有依赖改为精确目标 |
| PLATFORM-02 | gapi 并入 render_vulkan | Platform 包不再查找 Vulkan |
| PLATFORM-03 | Window Core/Backend/Surface 拆分 | Input/Window/Render 样例独立 |
| CORE-FINAL | 删除旧 target/include/namespace | 架构扫描无例外 |

## 12. 验收闸门

- [ ] `modules/core` 不定义 Entity、Scene、Extension。
- [x] `modules/core` 公共目标不链接 EnTT。
- [ ] `lux::serialization` 不链接 Engine Reflection Registry。
- [ ] `modules/platform` 不包含 Vulkan Object Wrapper。
- [ ] `lux::window` 公共头不 include `<vulkan/vulkan.h>`。
- [ ] `lux::window` 不 PUBLIC 链接 GLFW。
- [ ] `platform/common` target 已删除。
- [ ] `resource/spatial` target 已删除。
- [ ] `extension_abi` 不出现在 Modules SDK 安装清单。
- [ ] 所有 Build-time 生成器依赖不进入安装 Config。
