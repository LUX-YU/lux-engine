# CMake、命名空间、SDK 包与兼容迁移

> 把仓库内部层级从外部 API 中移除，重建安装包、Target、Include Prefix、依赖分类与自动架构检查

**执行文档 09 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | CMake、发布、SDK、持续集成、ABI 与所有模块负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** 本轮不创建 `lux/asset/AssetId.hpp`、新 `AssetTypeId` 或其兼容 shim。Scene 安装面收敛为单一 `lux-engine-scene` component `scene`；旧 `scene_api`、`scene_package` target/include/component 一次删除。全局 M7 package rename 仍不推进。

> **2026-08-21 裁决更新：** 本轮仅在现有 `lux::engine::resource::asset` target 内迁移领域头路径，不推进全局 M7。新增 `lux::engine::content` / `lux-engine-content` `content` component；旧 Asset `codecs/`、`pak/`、根部领域头与 Resource `BuiltinAssetIds.hpp` 不保留兼容层。


## 1. 目标

外部用户应按领域消费：

```cmake
find_package(lux-render CONFIG REQUIRED)
target_link_libraries(app PRIVATE lux::render lux::render_vulkan)
```

而不是理解仓库层级：

```cmake
find_package(lux-engine-function COMPONENTS render_client render_vulkan)
target_link_libraries(app PRIVATE
    lux::engine::function::render_vulkan)
```

内部层级分类仍可存在，但不进入用户 API。

## 2. Target 映射

| 旧目标 | 新目标 | 分发 |
| --- | --- | --- |
| `lux::engine::core::events` | `lux::events` | `lux-core` |
| `lux::engine::core::log` | `lux::log` | `lux-core` |
| `lux::engine::core::math` | `lux::math` | `lux-core` |
| `lux::engine::core::serialization` | `lux::serialization` | `lux-core` |
| `lux::engine::core::meta` | 删除；使用 `lux::cxx::reflection_runtime`/ECS adapter | 不再独立公共包 |
| `lux::engine::core::extension_abi` | `lux::engine::extensions_api` | `lux-engine-extension-sdk` |
| `lux::engine::platform::common` | 删除 | — |
| `lux::engine::platform::window` | `lux::window` / `lux::window_glfw` | `lux-platform` |
| `lux::engine::platform::gapi` | 删除；并入 `lux::render_vulkan` | `lux-render` |
| `lux::engine::platform::dynamic_library` | `lux::dynamic_library` | `lux-platform` |
| `lux::engine::platform::filewatch` | `lux::filewatch` | `lux-platform` |
| `lux::engine::resource::description` | `lux::description` | `lux-resource` |
| `lux::engine::resource::asset_identity` | 合入 `lux::asset` | `lux-resource` |
| `lux::engine::resource::asset_core` | `lux::asset` + `lux::engine::assets` | 拆分 |
| `lux::engine::resource::asset_codecs` | `lux::asset` 的 standard codec registration | `lux-resource` |
| `lux::engine::resource::asset_pak` | `lux::asset_pak` | `lux-resource` |
| `lux::engine::resource::deployment` | 删除；分到 Render Config 与 Game Deployment | — |
| `lux::engine::resource::entity_scene` | `lux::ecs::scene_format` + `lux::engine::scene_package` | 拆分 |
| `lux::engine::resource::spatial` | 合入 `lux::math` | `lux-core` |
| `lux::engine::function::render_client` | `lux::render` | `lux-render` |
| `lux::engine::function::render_graph` | `lux::render_graph` | `lux-render` |
| `lux::engine::function::render_vulkan` | `lux::render_vulkan` | `lux-render` |
| `lux::engine::function::render_features` | `lux::render_standard` | `lux-render` |
| `lux::engine::function::input` | `lux::input` | `lux-input` |
| `lux::engine::function::animation` | `lux::animation` | `lux-animation` |
| `lux::engine::function::navigation` | `lux::navigation` | `lux-navigation` |
| `lux::engine::function::script_core` | `lux::script` | `lux-script` |
| `lux::engine::function::script_lua` | `lux::script_lua` | `lux-script` |
| `lux::engine::function::script_native` | `lux::script_native` | `lux-script` |
| `lux::engine::function::ui` | `lux::ui` / `lux::ui_imgui` | `lux-ui` |
| `lux::engine::function::ui_vulkan` | `lux::ui_render_vulkan` | `lux-ui`/`lux-render` integration |
| `lux::engine::runtime::runtime_execution` | `lux::engine::execution` | Engine internal |
| `lux::engine::runtime::runtime_assets` | `lux::engine::assets` | Engine internal |
| `lux::engine::runtime::runtime_scene_core` | `lux::engine::scene` | Engine internal |
| `lux::engine::runtime::runtime_extension_loader` | `lux::engine::extensions_loader` | Engine internal |
| `lux::engine::runtime::runtime_render_backend_host` | 删除；`lux::render` | `lux-render` |
| `lux::engine::host::game_application` | `lux::game` | Engine product library |

## 3. Include Prefix 迁移

### 3.1 规则

公共 modules：

```text
lux/<domain>/...
```

ECS：

```text
lux/ecs/...
```

Engine：

```text
lux/engine/<domain>/...
```

产品：

```text
lux/game/...
lux/editor/...
```

### 3.2 示例

```text
lux/engine/resource/asset/AssetId.hpp
→ lux/asset/AssetId.hpp

lux/engine/description/Mesh.hpp
→ lux/description/Mesh.hpp

lux/engine/function/render/client/RenderTypes.hpp
→ lux/render/Types.hpp

lux/engine/window/LuxWindow.hpp
→ lux/platform/Window.hpp 或 lux/window/Window.hpp

lux/engine/runtime/scene/SceneRuntime.hpp
→ lux/engine/scene/Scene.hpp
```

### 3.3 Forwarding Header

生成而不是手写大量转发头。CREATE：

```text
cmake/Compatibility/GenerateForwardingHeaders.cmake
```

输入映射：

```cmake
lux_add_forwarding_header(
    OLD lux/engine/resource/asset/AssetId.hpp
    NEW lux/asset/AssetId.hpp
    REMOVE_AFTER 2.0)
```

生成内容只允许：

```cpp
#pragma once
#if defined(_MSC_VER)
#pragma message("deprecated: include <lux/asset/AssetId.hpp>")
#endif
#include <lux/asset/AssetId.hpp>
```

## 4. Package 设计

### 4.1 Modules SDK

```text
lux-core
lux-platform
lux-resource
lux-render
lux-input
lux-animation
lux-navigation
lux-script
lux-ui
```

### 4.2 Engine SDK

按实际外部嵌入需求安装：

```text
lux-engine-ecs
lux-engine-scene
lux-engine-game
lux-engine-extension-sdk
```

Editor、Toolchain 默认是产品/工具，不自动作为通用 SDK。

### 4.3 CPack 组件

```text
lux_modules_sdk
lux_ecs_sdk
lux_engine_sdk
lux_extension_sdk
lux_player
lux_editor
lux_toolchain
```

`lux_modules_sdk` 不包含：

```text
Scene
Extension ABI
Game
Editor
ECS
```

## 5. `install_components` 重构

当前每个层建立一个 `lux-engine-*` Package 并枚举大量细碎 component。目标改为领域包。

建议新增通用函数：

```cmake
lux_install_package(
    PACKAGE lux-render
    NAMESPACE lux::
    TARGETS
        lux_render
        lux_render_graph
        lux_render_vulkan
        lux_render_standard
)
```

函数必须：

- 生成 relocatable Config；
- 不写入 build-tree absolute path；
- 递归收集外部依赖；
- 区分 Build Tool 与 Runtime Dependency；
- 支持 COMPONENT；
- 生成 version file；
- 运行 installed-package smoke test。

## 6. Alias 迁移

### 6.1 原则

真实目标使用新名称：

```cmake
add_library(lux_render ...)
add_library(lux::render ALIAS lux_render)
```

旧目标只能：

```cmake
add_library(lux::engine::function::render_client ALIAS lux_render)
```

不能反过来让新目标 alias 旧目标，否则安装导出仍以旧语义为中心。

### 6.2 Imported Compatibility Package

旧 `find_package(lux-engine-function)` 可在一个版本内生成 compatibility config，它内部查找 `lux-render/lux-input/...` 并创建 deprecated imported aliases。

删除时间写入：

```text
doc/migration/legacy-package-removal.md
```

## 7. Target 分类 DAG

### 7.1 目标层

建议分类枚举：

```text
MODULE_CORE
MODULE_PLATFORM
MODULE_RESOURCE
MODULE_FUNCTION
ECS
ENGINE
AUTHORING
TOOLCHAIN
EDITOR
PRODUCT
TEST
BUILD_TOOL
```

### 7.2 合法边

```text
MODULE_* → 更低 MODULE_*
ECS → MODULE_*
ENGINE → ECS + MODULE_*
AUTHORING → MODULE_* + 可选 ECS schema
TOOLCHAIN → AUTHORING + 格式所有者
EDITOR → ENGINE + AUTHORING + TOOLCHAIN
PRODUCT → 对应产品库 + Platform backend
BUILD_TOOL → 可依赖需要解析的 SDK，但不得进入 Runtime closure
```

### 7.3 禁止边

```text
MODULE_* → ECS/ENGINE/EDITOR/TOOLCHAIN
ECS → ENGINE/EDITOR
ENGINE core → EDITOR/TOOLCHAIN
Game → Editor/Authoring UI
Render → ECS
```

## 8. 架构检查脚本

### 8.1 Include 扫描

CREATE `tools/architecture/check_includes.py`：

```python
FORBIDDEN = {
    "modules": [
        "lux/engine/runtime/",
        "lux/engine/editor/",
        "lux/engine/hosts/",
        "lux/ecs/",
    ],
    "ecs": [
        "lux/engine/editor/",
        "lux/game/",
    ],
}
```

扫描：

- public headers；
- source includes；
- generated headers；
- forwarding headers单独 allowlist。

### 8.2 CMake Link Closure

CREATE `tools/architecture/check_target_graph.py`，输入 CMake File API codemodel JSON：

```text
build/.cmake/api/v1/reply/target-*.json
```

检查每个 target 的递归依赖分类。

### 8.3 安装闭包

对安装前缀执行：

```text
Config package 搜索
动态库依赖 inventory
头文件 forbidden token
build path leak
```

### 8.4 用户语义扫描

对导出游戏目录拒绝：

```text
editor
toolchain
authoring
EngineRuntime
EditorRuntime
engine_pak
```

对公共 Modules SDK 拒绝：

```text
SceneRuntime
GameApplication
EngineExtensions
RuntimeContributionRegistrar
```

## 9. Build-time Tool 处理

### 9.1 Host Tools

跨编译时，Meta Generator、Shader Emitter、Asset Packer 使用 `LUX_HOST_TOOLS_PREFIX`。公共 Runtime Config 只需要生成结果，不在消费者机器重新运行生成器，除非用户主动使用 Toolchain SDK。

### 9.2 CMake Script 归属

| 当前 | 目标 |
| --- | --- |
| `engine_add_meta.cmake` | `cmake/Codegen/Reflection.cmake` |
| render comm operation scripts | `cmake/Codegen/RenderOperations.cmake` |
| shader emit scripts | `lux-render-toolchain` 或 build tool package |
| `add_asset.cmake` | `lux-resource` 的 Asset Build Helpers |
| runtime inventory | products/export tooling |

Build Helper Package 与 Runtime Library Package 分开安装。

## 10. Profile 重构

保持现有：

```text
DEVELOPER
PLAYER
EDITOR
TOOLCHAIN
```

新增：

```text
MODULES_SDK
ECS_SDK（可选）
```

Profile 必须用显式目录列表，不使用自动目录枚举。

### 10.1 Profile 矩阵

| Profile | Modules | ECS | Engine | Authoring | Toolchain | Editor | Products |
| --- | --- | --- | --- | --- | --- | --- | --- |
| MODULES_SDK | ✓ | — | — | — | — | — | samples |
| PLAYER | ✓ | ✓ | ✓ | — | — | — | player |
| EDITOR | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | editor |
| TOOLCHAIN | 最小集 | schema 需要 | format 需要 | ✓ | ✓ | — | tools |
| DEVELOPER | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | all |

## 11. 输出文件名

外部库：

```text
lux_render
lux_asset
lux_input
```

Engine 产品内部：

```text
lux_engine_scene
lux_engine_game
lux_editor
```

导出游戏可执行文件使用项目名，而不是 `lux_player`。参考 Player 可执行仍可叫 `lux_player`，Exporter 复制/重命名为游戏名。

## 12. ABI 与 Symbol Visibility

### 12.1 Visibility Header

每个公开包生成领域路径：

```text
lux/render/visibility.hpp
lux/asset/visibility.hpp
```

不再复用：

```text
lux/engine/function/visibility.h
lux/engine/resource/visibility.h
```

### 12.2 ABI 测试

Extension ABI：

```text
sizeof
alignof
standard_layout
symbol names
fingerprint
```

Render/Asset 公共 C++ ABI 若不承诺跨编译器稳定，应在文档中明确“同 toolchain version”合同，不假装稳定 C ABI。

## 13. 迁移脚本

CREATE：

```text
tools/migration/rewrite_includes.py
tools/migration/rewrite_targets.py
tools/migration/report_legacy_symbols.py
```

脚本只执行确定映射；无法确定的 include 生成报告，不自动猜测。

每次脚本运行后：

```text
clang-format
CMake configure
build
tests
legacy scan
```

## 14. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| BUILD-01 | 新分类与 File API graph checker | 旧图可检查 |
| BUILD-02 | 新领域 Alias，不改 package | 全构建 |
| BUILD-03 | 新 Include Prefix + forwarding headers | 旧消费者可编译 |
| BUILD-04 | Modules SDK packages | installed samples |
| BUILD-05 | Engine/ECS package 重组 | profiles |
| BUILD-06 | Build Tool package 分离 | cross compile |
| BUILD-07 | 迁移仓内所有 include/targets | legacy report 仅兼容 |
| BUILD-FINAL | 删除旧 Config/Alias/Forwarding | legacy report 为零 |

## 15. 验收闸门

- [ ] 外部 Render 样例只使用 `find_package(lux-render)`。
- [ ] 外部 Asset 样例只使用 `find_package(lux-resource)`。
- [ ] 公共 headers 使用 `lux/<domain>`。
- [ ] 旧 target 只是 alias，不是真实实现 target。
- [ ] Modules SDK Config 无 ECS/Engine。
- [ ] Build Tool 依赖不进入 Runtime Config。
- [ ] CMake File API 依赖图检查在持续集成运行。
- [ ] 安装前缀无 build-tree absolute path。
- [ ] Legacy include/target report 最终为零。
- [ ] 导出游戏 inventory 无 Editor/Toolchain/Authoring。
