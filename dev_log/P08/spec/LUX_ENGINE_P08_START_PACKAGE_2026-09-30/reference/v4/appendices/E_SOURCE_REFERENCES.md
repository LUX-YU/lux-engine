# 源码与证据边界

基准固定为 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`。本次通过 GitHub 连接查询 main 与关键头/CMake，未取得可构建完整 checkout，未执行引擎编译/CTest/GPU，也没有修改远程仓库。下列实现建议是目标契约，不应以“已实现”的口吻引用。对未重新逐行读取的缺陷采用 V3 调查结论，并要求 P00 在实际源树中验证；条件性风险不宣称已经复现。

| 引用 | 资料 | 核对范围 |
| --- | --- | --- |
| S00 | [提交及默认分支](https://github.com/LUX-YU/lux-engine/commit/2bb33ff1a1f11025cf404e074c0e9b259239d8a4) | 本次重新查询 main 未变 |
| S01 | [Editor Impl](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/pinclude/lux/engine/editor/detail/EditorImpl.hpp) | 前序源码审阅/V3 |
| S02 | [EditorContext](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/EditorContext.hpp) | 已核对固定提交 |
| S03 | [PaneManager](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/PaneManager.hpp) | 本次读取 |
| S04 | [SceneEditor](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/include/lux/engine/editor/scene/SceneEditor.hpp) | 前序源码审阅 |
| S05 | [Scene Impl](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneEditorImpl.hpp) | 前序全文与本次尾部读取 |
| S06 | [Material Impl](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/pinclude/lux/engine/editor/material/MaterialEditorImpl.hpp) | 本次读取 |
| S07 | [FlowForge Impl](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/flowforge/pinclude/lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp) | 本次读取 |
| S08 | [EditHistory](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditHistory.hpp) | 前序源码审阅 |
| S09 | [EditTypes](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditTypes.hpp) | 本次读取 |
| S10 | [SceneRuntime](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp) | 前序源码审阅 |
| S11 | [Pane](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/modules/function/ui/include/lux/engine/ui/Pane.hpp) | 本次读取 |
| S12 | [Element](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/modules/function/ui/include/lux/engine/ui/Element.hpp) | 本次读取 |
| S13 | [UI Ids](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/modules/function/ui/include/lux/engine/ui/Ids.hpp) | 本次读取 |
| S14 | [LuxObject](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/modules/core/object/include/lux/engine/object/LuxObject.hpp) | 本次读取前150行；析构实现待P00核验 |
| S15 | [WorldObjectId](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/engine/domain/world/identity/include/lux/engine/world/WorldObjectId.hpp) | 本次搜索确认正确路径和UUID身份 |
| S16 | [UI CMake](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/CMakeLists.txt) | 本次读取 |
| S17 | [Scene CMake](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/CMakeLists.txt) | 本次读取 |
| S18 | [History CMake](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/CMakeLists.txt) | 本次读取 |
| S19 | [顶层 CMake](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/CMakeLists.txt) | 本次读取 |
| S20 | [WorkspaceRequest](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/WorkspaceRequest.hpp) | 前序源码审阅 |
| S21 | [PaneRegistration](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/metadata/include/lux/engine/editor/metadata/PaneRegistration.hpp) | 前序源码审阅 |
| S22 | [CommandRegistration](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/metadata/include/lux/engine/editor/metadata/CommandRegistration.hpp) | 前序源码审阅 |
| S23 | [Layout restore](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorWorkspace.cpp) | 前序源码审阅/V3 |
| S24 | [Layout storage](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorWorkspaceStorage.cpp) | 前序源码审阅/V3 |
| S25 | [Menu query/execute](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorMenu.cpp) | 前序源码审阅/V3 |
| S26 | [Startup](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorStartup.cpp) | 前序源码审阅/V3 |
| S27 | [App CMake](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/CMakeLists.txt) | 前序源码审阅/V3 |
| S28 | [CI](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/.github/workflows/deb.yml) | 前序源码审阅/V3 |

CMake 的 PUBLIC/PRIVATE/INTERFACE 与链接传递规则参考 [官方 target_link_libraries 文档](https://cmake.org/cmake/help/latest/command/target_link_libraries.html)。文档中的原地目录重组不要求升级当前 C++20 或 CMake 最低版本。

V3 原件：[V3_REFERENCE.md](../references/V3_REFERENCE.md)。保留它作为完整概念与 Q 测试来源；执行顺序、具体期限和明确差异以本实施包为准。
