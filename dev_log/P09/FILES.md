# P09 文件变更

前置 `41167a9bdff8c192fe990d53aa8dfa132d57b081` → 实现 `38c88aeaf14ca27687d1baf844b4cc2d28cfcfe1`。

## 新增（19）

- `cmake/installed-consumers/workspace/CMakeLists.txt`
- `editor/contracts/include/lux/engine/editor/views/ViewInfo.hpp`
- `editor/tests/architecture/test_workspace_boundaries.py`
- `editor/tests/workspace/CMakeLists.txt`
- `editor/tests/workspace/effects.cpp`
- `editor/tests/workspace/workspace.cpp`
- `editor/workspace/README.md`
- `editor/workspace/layout/CMakeLists.txt`
- `editor/workspace/layout/include/lux/engine/editor/workspace/DockLayout.hpp`
- `editor/workspace/layout/include/lux/engine/editor/workspace/LayoutCatalog.hpp`
- `editor/workspace/layout/include/lux/engine/editor/workspace/LayoutPlan.hpp`
- `editor/workspace/layout/include/lux/engine/editor/workspace/WorkspaceValues.hpp`
- `editor/workspace/layout/src/LayoutPlan.cpp`
- `editor/workspace/layout/src/WorkspaceCodec.cpp`
- `editor/workspace/recovery/include/lux/engine/editor/workspace/RecoveryManifest.hpp`
- `editor/workspace/storage/CMakeLists.txt`
- `editor/workspace/storage/include/lux/engine/editor/workspace/WorkspaceStore.hpp`
- `editor/workspace/storage/src/LegacyWorkspaceImporter.cpp`
- `editor/workspace/storage/src/WorkspaceStore.cpp`

## 修改（9）

- `editor/CMakeLists.txt`
- `editor/adapters/project_io/src/ProjectArtifactStore.cpp`
- `editor/contracts/CMakeLists.txt`
- `editor/persistence/include/lux/engine/editor/persistence/ArtifactStore.hpp`
- `editor/persistence/include/lux/engine/editor/persistence/WriteCoordinator.hpp`
- `editor/persistence/src/WriteCoordinator.cpp`
- `editor/tests/architecture/CMakeLists.txt`
- `editor/tests/architecture/check_editor_boundaries.py`
- `editor/tests/architecture/rules.json`

## 删除（1）

- `editor/views/api/include/lux/engine/editor/views/ViewInfo.hpp`
