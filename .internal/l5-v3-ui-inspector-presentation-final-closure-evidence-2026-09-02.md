# L5 v3 UI / Inspector / Presentation Final Closure Evidence

Date: 2026-09-02

Status: **PASS — Gate U, Gate C and visible Editor presentation closed on Windows; Android build and UI texture
registration are not claimed**

Qualified source revision: `d744896556522fd35f91d7302edc6f7a19cc42de`

## Final repairs

- `DropTargetScope::accept()` now accepts inside its already-active backend target scope. The Viewport one-shot helper
  remains the only Begin/Accept/End path.
- `UiVulkanPresentation` maps `std::jthread` allocation and `std::system_error` failures to structured create errors;
  no thread exception can cross its `noexcept` factory.
- UI draw submission payload and attachment type ownership are private to the Editor presentation implementation.
  Generic installed Render headers contain no ImGui draw-data vocabulary.

## Clean tracked snapshot

An independent detached worktree was created from the qualified revision. It reported an empty porcelain status and:

```text
cmake -DLUX_SOURCE_DIR=<clean-worktree> -P cmake/ValidateTrackedSnapshot.cmake
-- Tracked snapshot is clean: d744896556522fd35f91d7302edc6f7a19cc42de
```

The primary worktree's existing `.gitignore` and `WorldPartition.hpp` changes were not included.

## Windows build and test matrix

Each profile used a fresh build tree and full `all -j 4 -k 0`. CTest ran inside the MSVC `vcvars64` environment.

```text
Default Developer: 166/166 CTest passed
EDITOR:            179/179 CTest passed
Full Render:       182/182 CTest passed
```

Full Render used `LUX_BUILD_PACKED_RENDER_CONTENT=ON` and includes texture/model Vulkan qualification, L1-to-L3
render sync and large-scene performance. All three profiles received a second full build and reported
`ninja: no work to do`.

`ui_drag_drop_scope_test`, `editor_presentation_thread_start`, `editor_application` and
`editor_entity_inspector` each passed 50 consecutive repeat-until-fail runs.

## Installed and architecture closure

- Source architecture validation passed in all three builds.
- A fresh EDITOR prefix passed `ValidateInstalledArchitecture.cmake`.
- The relocated installed Lux UI consumer passed 1/1 CTest.
- The relocated installed EntityInspector consumer configured, linked and exited 0.
- Installed generic Render headers contain zero matches for `SubmitImGuiDrawDataPayload`, `ImGuiDrawData` or
  `ImGuiCommConfig`.
- Changed Render public headers were synchronized to Debug, RelWithDebInfo and Android install include prefixes.

## Presentation qualification

```text
lux_editor --hidden --frames 300 --validation
exit 0
```

A visible validation-enabled run completed 900x600 and 1200x800 resize, minimize/restore, Transform drag,
Inspector Undo/Redo input, enum popup input and window close; the process exited 0. Automated Inspector tests remain
the authoritative proof for patch, atomic choice and undo/redo semantics.

## Explicit boundaries

- No AssetBrowser, SceneOutliner, SceneViewport, NodeCanvas, MaterialEditor or FlowForgeEditor was added.
- Non-zero `TextureHandle` values still have no Vulkan descriptor registration seam. This remains deferred until the
  first D1 thumbnail or E2 Viewport consumer.
- Android install headers were synchronized, but Android configure/build/CTest was not run.
- The next implementation wave is E1 Flat SceneOutliner; this closure does not begin E1.
