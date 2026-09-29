# P07 文件变更

基线 `38fd05d551e4556f9d713b793de05f98db1bd156` → 实现 `2997f7e87eb2634e64fa9c258251c8128c8bacec`。完整 Git 内容哈希见 files.json。

## 新增（31）

- `cmake/installed-consumers/projection-compilation/CMakeLists.txt`
- `editor/tests/architecture/test_projection_compilation_boundaries.py`
- `editor/tools/flowforge/compilation/CMakeLists.txt`
- `editor/tools/flowforge/compilation/README.md`
- `editor/tools/flowforge/compilation/include/lux/engine/editor/flowforge/FlowCompilationService.hpp`
- `editor/tools/flowforge/compilation/include/lux/engine/editor/flowforge/PublishFlowArtifact.hpp`
- `editor/tools/flowforge/compilation/src/FlowCompilationService.cpp`
- `editor/tools/flowforge/compilation/src/PublishFlowArtifact.cpp`
- `editor/tools/material/preview/CMakeLists.txt`
- `editor/tools/material/preview/README.md`
- `editor/tools/material/preview/include/lux/engine/editor/material/MaterialCompilation.hpp`
- `editor/tools/material/preview/include/lux/engine/editor/material/MaterialPreviewStore.hpp`
- `editor/tools/material/preview/include/lux/engine/editor/material/PublishCompiledMaterial.hpp`
- `editor/tools/material/preview/src/MaterialCompilation.cpp`
- `editor/tools/material/preview/src/MaterialPreviewStore.cpp`
- `editor/tools/material/preview/src/PublishCompiledMaterial.cpp`
- `editor/tools/material/preview/test/compilation.cpp`
- `editor/tools/scene/projection/CMakeLists.txt`
- `editor/tools/scene/projection/README.md`
- `editor/tools/scene/projection/include/lux/engine/editor/scene/ResourceStatus.hpp`
- `editor/tools/scene/projection/include/lux/engine/editor/scene/SceneProjection.hpp`
- `editor/tools/scene/projection/include/lux/engine/editor/scene/ViewportPresentation.hpp`
- `editor/tools/scene/projection/src/HighlightRenderer.hpp`
- `editor/tools/scene/projection/src/ResourceStatus.cpp`
- `editor/tools/scene/projection/src/SceneProjection.cpp`
- `editor/tools/scene/projection/src/ViewportPresentation.cpp`
- `editor/tools/scene/projection/test/highlight.cpp`
- `editor/tools/scene/projection/test/projection.cpp`
- `editor/transition/FlowCompilationAccess.hpp`
- `editor/transition/MaterialCompilationAccess.hpp`
- `modules/function/render/features/test/view_binding.cpp`

## 修改（34）

- `cmake/EditorArchitectureChecks.cmake`
- `editor/CMakeLists.txt`
- `editor/assets/sinclude/lux/engine/editor/detail/AssetSave.hpp`
- `editor/tests/architecture/CMakeLists.txt`
- `editor/tests/architecture/check_editor_boundaries.py`
- `editor/tests/architecture/rules.json`
- `editor/tools/flowforge/CMakeLists.txt`
- `editor/tools/flowforge/pinclude/lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp`
- `editor/tools/flowforge/src/FlowAssets.cpp`
- `editor/tools/flowforge/src/FlowForgeEditor.cpp`
- `editor/tools/flowforge/src/ui/FlowForgeElements.cpp`
- `editor/tools/material/CMakeLists.txt`
- `editor/tools/material/pinclude/lux/engine/editor/material/MaterialEditorImpl.hpp`
- `editor/tools/material/src/MaterialAssets.cpp`
- `editor/tools/material/src/MaterialEditor.cpp`
- `editor/tools/material/src/MaterialPreview.cpp`
- `editor/tools/material/src/ui/MaterialElements.cpp`
- `editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneEditorImpl.hpp`
- `editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneOpening.hpp`
- `editor/tools/scene/src/SceneAssets.cpp`
- `editor/tools/scene/src/SceneEditor.cpp`
- `editor/tools/scene/src/SceneOpening.cpp`
- `editor/tools/scene/src/ui/SceneContentElement.cpp`
- `editor/ui/CMakeLists.txt`
- `editor/ui/include/lux/engine/editor/ui/SceneElement.hpp`
- `editor/ui/src/SceneElement.cpp`
- `modules/function/render/features/include/lux/engine/function/render/features/grid/Grid3DOperation.hpp`
- `modules/function/render/features/include/lux/engine/function/render/features/highlight/HighlightOperation.hpp`
- `modules/function/render/features/sinclude/lux/engine/render/renderer/features/grid/Grid3DPassFeature.hpp`
- `modules/function/render/features/sinclude/lux/engine/render/renderer/features/highlight/HighlightFeature.hpp`
- `modules/function/render/features/src/assembly/grid/Grid3DOperationHandlers.cpp`
- `modules/function/render/features/src/assembly/highlight/HighlightOperationHandlers.cpp`
- `modules/function/render/features/src/renderer/features/grid/Grid3DPassFeature.cpp`
- `modules/function/render/features/src/renderer/features/highlight/HighlightFeature.cpp`

## 删除（3）

- `editor/tools/flowforge/pinclude/lux/engine/editor/flowforge/FlowCompilation.hpp`
- `editor/tools/material/pinclude/lux/engine/editor/material/MaterialCompilation.hpp`
- `editor/tools/material/pinclude/lux/engine/editor/material/MaterialPreview.hpp`
