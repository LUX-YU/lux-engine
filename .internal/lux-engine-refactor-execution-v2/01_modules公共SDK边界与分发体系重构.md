# `modules/` 公共 SDK 边界与分发体系重构

> 把 modules 从“依赖图底层”恢复为可独立采用的库产品，并建立可自动验证的公共边界

**执行文档 01 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | 公共 SDK 负责人、CMake 维护者、Render/Input/Resource 等模块负责人、发布工程师 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** 公共 SDK 可以拥有与 Engine 语义无关的完整资产机制。现有 `asset_id_t`、`LuxAsset`、`AssetManager`、Codec Catalog、Provider、VFS 与 Pak 保留在 `modules/resource/asset`；旧版提出的 `AssetId`/`AssetTypeId` 改名和 AssetStore 上移目标由 `ADR-20260820_SceneAsset与Resource边界.md` 取代。

> **2026-08-21 裁决更新：** Provider/VFS 是 opaque bytes 存储面，Pak v2 的 reader/writer/inspector 都是公共 Asset SDK；Asset 源码按 texture/material/mesh/model/animation/shader/script/storage 领域内聚。冻结的 Engine 内置资产 ID 不属于 Modules。详见 `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md`。

> **2026-08-21 Profile 修订：** 不新增 `MODULES_SDK` Profile。公共 SDK 独立性通过现有四个 Profile 的安装结果和 installed consumers 验证；本文后续 `MODULES_SDK` 示例仅为历史提案，不再施工。Modules 聚合目录仍必须改为显式子目录列表。

> **2026-08-21 Extension ABI 实施状态：** `modules/core/extension_abi` 已删除；ABI v4 只由独立 `lux-engine-extensions` package 的 `extension_api` component 安装，Core/Modules package 不再导出 Extension API。


## 1. 当前问题与施工目标

当前 README 将 `modules/platform → core → resource → function` 描述为 ECS 与 Engine 之下的复用层，这一方向正确；但实际目录已经混入 Engine Extension ABI、Game Deployment Manifest、ECS Entity Scene、World Partition 配置、Runtime Reflection Entity Registry 等上层语义。

本工作流的目标不是把这些目录重新命名，而是建立以下可执行合同：

```text
只安装 modules SDK
→ 能配置和编译独立 Render/Input/Asset/UI 示例
→ 不配置 ecs/
→ 不配置 engine/
→ 不出现 SceneRuntime、EngineExtensions、EditorPanel、GameApplication
```

### 1.1 当前目录裁决

| 当前目录 | 动作 | 目标公开面 | 说明 |
| --- | --- | --- | --- |
| `modules/core/events` | 保留 | `lux::events` | 清理对 Host/AsyncRuntime 的说明；API 只表达延迟事实分发 |
| `modules/core/log` | 保留 | `lux::log` | Sink 接口保持可注入；Engine 异步 Sink 留在 Engine |
| `modules/core/math` | 保留并扩充 | `lux::math` | 吸收 `resource/spatial` 的纯值类型 |
| `modules/core/serialization` | 拆分 | `lux::serialization` | 只保留 Archive/NameTable/Byte primitives；反射 Component Archive 上移 `ecs/serialization` |
| `modules/core/meta` | 拆除聚合边界 | `lux-cxx::reflection_runtime` + `ecs` + `engine/reflection` | EntityRegistry 与 Extension Sidecar 语义移出 |
| `modules/core/extension_abi`（已删除） | 已搬迁 | `engine/extensions/api` | DONE；作为单独 Engine Extension SDK，而非公共 Core |
| `modules/platform/common` | 删除 | 分别迁移 | AtomicWait/Format/Size2D/ImageEnums 各归其领域 |
| `modules/platform/dynamic_library` | 保留 | `lux::dynamic_library` | 独立 RAII 动态库加载 |
| `modules/platform/filewatch` | 保留 | `lux::filewatch` | Editor 只是当前消费者，不改变通用性 |
| `modules/platform/window` | 拆分 | `lux::window`、`lux::window_glfw` | 核心抽象、平台 Backend、Vulkan Surface Integration 分离 |
| `modules/platform/gapi` | 搬迁 | `modules/function/render/vulkan/low_level` | 实际是 Vulkan Wrapper，不是 Platform 抽象 |
| `modules/resource/description` | 保留并纯化 | `lux::description` | 只保留被动资源值类型 |
| `modules/resource/asset` | 保留并重构 | `lux::asset` | 通用 Asset/SerDeser/Catalog/Manager/Provider/VFS 与 Pak 读写；不含 Engine 默认内容 |
| `modules/resource/*` 其他目录 | 消除一级组件 | 按语义迁移 | 见文档 03 |
| `modules/function/render` | 保留并重构 | `lux::render*` | 公共渲染库主边界 |
| `modules/function/input` | 保留并解耦 | `lux::input` | 不 PUBLIC 依赖 GLFW/Window Backend |
| `modules/function/animation` | 保留并解耦 | `lux::animation` | 依赖 Description，不依赖 AssetStore |
| `modules/function/navigation` | 保留 | `lux::navigation` | 依赖 Math/Spatial 值 |
| `modules/function/script` | 保留 | `lux::script*` | `ScriptModule` 仅指脚本模块 |
| `modules/function/ui` | 拆分 | `lux::ui*` | 基础 UI、ImGui、GLFW、Vulkan、Editor Viewport 分离 |

## 2. 公共 SDK 准入标准

一个目标只有同时满足以下条件，才允许安装为 `modules/` 公共组件：

1. 名称是外部用户能直接理解的领域名称；
2. 公共头文件不包含 `lux/engine/runtime`、`lux/engine/editor`、`lux/engine/hosts`；
3. PUBLIC Link 闭包不包含 `ecs`、`engine/runtime`、`engine/editor`、`engine/toolchain`；
4. 不依赖 Lux Engine Extension ABI；
5. 不依赖具体产品 Manifest；
6. 具有独立外部使用场景；
7. 具有独立测试；
8. 安装后的 Config Package 能单独 `find_package`；
9. 公共 ABI 不暴露私有 Backend 类型；
10. Build-time Generator 不通过 PUBLIC Link 泄漏到消费者。

### 2.1 不足以成为公共组件的理由

以下理由单独存在时均不成立：

```text
“这个目录有自己的 CMakeLists.txt”
“这个 Blob 有独立 Codec”
“两个上层模块都使用”
“拆成组件后增量编译更快”
“这样 install_components 列表更清楚”
```

增量编译问题应使用 Object Library、Unity 分组、私有 Static Library 或源文件分区解决。

## 3. 目标安装与命名空间

### 3.1 目标包

第一阶段不要求立即拆成多个仓库，但安装包应按外部领域组织：

| 包 | 主要目标 | 不得携带 |
| --- | --- | --- |
| `lux-core` | `lux::events`、`lux::log`、`lux::math`、`lux::serialization` | ECS、Extension、AssetManager |
| `lux-platform` | `lux::window`、`lux::window_glfw`、`lux::dynamic_library`、`lux::filewatch` | Renderer、Game、Editor |
| `lux-resource` | `lux::description`、`lux::asset`、`lux::asset_pak` | Scene、Deployment、World Partition |
| `lux-render` | `lux::render`、`lux::render_graph`、`lux::render_vulkan`、`lux::render_standard` | ECS、Editor、Game Session |
| `lux-input` | `lux::input` | GLFW 类型、Window 对象 |
| `lux-animation` | `lux::animation` | AssetStore、ECS Animator |
| `lux-navigation` | `lux::navigation`、`lux::navigation_detour3d` | ECS NavigationSystem |
| `lux-script` | `lux::script`、`lux::script_lua`、`lux::script_native` | Engine Extension |
| `lux-ui` | `lux::ui`、`lux::ui_imgui`、可选 Backend Integration | SceneViewport、Editor Panel |

### 3.2 目标 Include Prefix

```cpp
#include <lux/math/Position.hpp>
#include <lux/asset/Reader.hpp>
#include <lux/render/Renderer.hpp>
#include <lux/input/ActionMapper.hpp>
#include <lux/script/Runtime.hpp>
```

迁移期 Forwarding Header：

```cpp
// old: lux/engine/resource/asset/AssetId.hpp
#pragma once
#include <lux/asset/AssetId.hpp>
```

Forwarding Header 必须：

- 只包含新头；
- 带弃用注释；
- 在 `SDK-COMPAT-REMOVE` 里程碑删除；
- 不被新代码 include。

## 4. 精确 CMake 修改

### 4.1 根 CMake 增加 SDK-only Profile

CREATE `cmake/Profiles/ModulesSdk.cmake`：

```cmake
set(LUX_BUILD_MODULES_SDK ON CACHE BOOL "" FORCE)
set(LUX_BUILD_ECS         OFF CACHE BOOL "" FORCE)
set(LUX_BUILD_ENGINE      OFF CACHE BOOL "" FORCE)
set(LUX_BUILD_PRODUCTS    OFF CACHE BOOL "" FORCE)
```

MODIFY 根 `CMakeLists.txt`，将当前目录添加逻辑改为显式门控：

```cmake
add_subdirectory(modules)

if(LUX_BUILD_ECS)
    add_subdirectory(ecs)
endif()

if(LUX_BUILD_ENGINE)
    add_subdirectory(engine)
endif()

if(LUX_BUILD_PRODUCTS)
    add_subdirectory(products)
endif()
```

若当前 Profile 系统不适合增加布尔门控，则新增 `MODULES_SDK` Profile；关键验收是配置过程中完全不访问 `ecs/` 与 `engine/`。

### 4.2 删除目录枚举式自动发现

当前多个层使用 `subdirectory_list(dir_list)` 自动添加所有子目录。公共 SDK 边界必须改为显式列表，否则新建一个实验目录就会自动进入安装闭包。

MODIFY：

```text
modules/core/CMakeLists.txt
modules/platform/CMakeLists.txt
modules/resource/CMakeLists.txt
modules/function/CMakeLists.txt
```

目标写法：

```cmake
add_subdirectory(events)
add_subdirectory(log)
add_subdirectory(math)
add_subdirectory(serialization)
```

禁止：

```cmake
subdirectory_list(dir_list)
foreach(subdir ${dir_list})
    add_subdirectory(${subdir})
endforeach()
```

### 4.3 分类属性与公开属性分离

`LAYER / PRODUCT / ROLE` 是仓库内部持续集成元数据，不应进入安装后的公共 Target 名称。保留：

```cmake
lux_classify_target(
    TARGET  render
    LAYER   FUNCTION
    PRODUCT SDK
    ROLE    DOMAIN
)
```

但公开 Alias 使用：

```cmake
add_library(lux::render ALIAS lux_render)
```

而不是：

```cmake
lux::engine::function::render_vulkan
```

### 4.4 安装包兼容策略

迁移期同时导出：

```cmake
lux::render
lux::engine::function::render_vulkan  # deprecated alias
```

旧 Alias 不能成为真实目标，必须是新目标的 ALIAS 或 imported forwarding target。

## 5. 目录级施工清单

### 5.1 CREATE

```text
cmake/Profiles/ModulesSdk.cmake
cmake/Architecture/ValidateModulesSdk.cmake
tests/sdk/modules_minimal/
tests/sdk/render_minimal/
tests/sdk/asset_minimal/
tests/sdk/input_minimal/
tests/sdk/script_minimal/
```

### 5.2 MODIFY

```text
CMakeLists.txt
readme.md
modules/CMakeLists.txt
modules/core/CMakeLists.txt
modules/platform/CMakeLists.txt
modules/resource/CMakeLists.txt
modules/function/CMakeLists.txt
cmake/TargetArchitecture.cmake
cmake/WriteRuntimeDependencyInventory.cmake
```

`readme.md` 的依赖图改为：

```text
modules/core
  ├─ modules/platform
  ├─ modules/resource
  └─ modules/function
           ↓
          ecs
           ↓
         engine
           ↓
        products
```

不再把 `Resource` 视为所有共享协议的默认归宿。

### 5.3 MOVE / DELETE

具体文件迁移由文档 02–06 执行；本文件先建立闸门，防止迁移期间继续新增错位代码。

## 6. SDK 架构扫描器

CREATE `cmake/Architecture/ValidateModulesSdk.cmake` 或等价 Python 脚本，检查所有 `modules/**/include`：

```text
禁止 include 前缀：
  lux/engine/runtime/
  lux/engine/editor/
  lux/engine/hosts/
  lux/engine/authoring/
  lux/engine/toolchain/
  lux/engine/ecs/

禁止公开符号词：
  SceneRuntime
  GameApplication
  LuxEditor
  EngineExtensions
  RuntimeContributionRegistrar
  EditorContributionRegistrar
```

例外不得通过全局 Allowlist；必须限定到具体过渡文件，并写明删除版本。

### 6.1 PUBLIC Link 闭包检查

对每个安装目标执行：

```cmake
get_target_property(_links target INTERFACE_LINK_LIBRARIES)
```

递归解析后拒绝：

```text
LAYER=ECS
LAYER=RUNTIME
LAYER=EDITOR
LAYER=TOOLCHAIN
LAYER=HOST
PRODUCT=PLAYER
PRODUCT=EDITOR
```

### 6.2 构建工具泄漏检查

检查安装后的 `*Config.cmake` 不包含：

```text
libclang
MLIR
LLVM
shaderc
meta_generator
lux_shader_emitter
engine_add_meta.cmake 的构建树绝对路径
```

## 7. 外部消费样例

### 7.1 Render-only

```cmake
cmake_minimum_required(VERSION 3.25)
project(render_sample LANGUAGES CXX)

find_package(lux-render CONFIG REQUIRED)
find_package(lux-platform CONFIG REQUIRED COMPONENTS window_glfw)

add_executable(render_sample main.cpp)
target_link_libraries(render_sample
    PRIVATE
        lux::render
        lux::render_vulkan
        lux::window_glfw)
```

样例不得链接：

```text
lux-engine-ecs
lux-engine-runtime
lux-engine-editor
lux-engine-extension-sdk
```

### 7.2 Asset-only

```cmake
find_package(lux-resource CONFIG REQUIRED COMPONENTS asset description)

add_executable(asset_inspect main.cpp)
target_link_libraries(asset_inspect PRIVATE lux::asset lux::description)
```

### 7.3 SDK-only 构建闸门

持续集成新增：

```bash
cmake -S . -B build/modules-sdk   -DLUX_BUILD_PROFILE=MODULES_SDK

cmake --build build/modules-sdk --target all
ctest --test-dir build/modules-sdk --output-on-failure
```

随后使用安装结果配置上述外部样例，而不是在源树内直接链接目标。

## 8. 公共 API 版本策略

每个公共模块建立：

```text
API_VERSION
ABI_VERSION
FILE_FORMAT_VERSION（若有）
```

三者不得混为一个版本。

例如：

```text
lux::asset API v2
Asset file header v3
Pak index v2
```

Extension ABI 不属于公共 Asset/Core 版本；它由 `lux-engine-extension-sdk` 独立管理。

## 9. Pull Request 序列

| PR | 内容 | 必须保持 |
| --- | --- | --- |
| SDK-01 | 增加 `MODULES_SDK` Profile 与隔离测试 | 不移动任何实现 |
| SDK-02 | 显式列举 modules 子目录，停止自动发现 | 全 Profile 配置一致 |
| SDK-03 | 引入新公共 Alias 与 Include Prefix | 旧 Alias 仍工作 |
| SDK-04 | 接入 Include/Link 闭包扫描 | 允许受控兼容例外 |
| SDK-05+ | 按文档 02–04 迁移错位模块 | 每个 PR 可独立回滚 |
| SDK-FINAL | 删除旧包名、旧 Include、例外清单 | 外部样例全部使用新 API |

## 10. 验收闸门

- [ ] `MODULES_SDK` 配置不执行 `add_subdirectory(ecs)`。
- [ ] `MODULES_SDK` 配置不执行 `add_subdirectory(engine)`。
- [ ] 安装结果中不存在 `SceneRuntime.hpp`、`EngineExtensions.hpp`、`GameApplication.hpp`。
- [ ] 公共目标的递归 Link Closure 不包含 ECS/Engine/Editor。
- [ ] Render-only 外部样例可以创建窗口、初始化 Vulkan Renderer 并清理。
- [ ] Asset-only 外部样例可以读取 Header、选择 Codec、读取 Pak。
- [ ] Input-only 外部样例不需要 GLFW。
- [ ] Script-only 外部样例不需要 Engine Extension ABI。
- [ ] 新公共头不使用 `lux/engine/function` 或 `lux/engine/resource` 前缀。
- [ ] 所有兼容 Alias 都有删除里程碑。
