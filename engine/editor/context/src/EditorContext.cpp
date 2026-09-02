#include <lux/engine/editor/EditorContext.hpp>

#include <utility>

namespace lux::editor
{
    EditorContext::EditorContext(EditorContextCreateInfo info) noexcept
        : toolset_(&info.toolset),
          vfs_(std::move(info.vfs)),
          asset_read_(std::move(info.asset_read)),
          execution_(&info.execution),
          tasks_(&info.tasks),
          selection_(&info.selection),
          ui_(&info.ui),
          scene_meta_(&info.scene_meta)
    {
    }

    Toolset& EditorContext::toolchain() noexcept { return *toolset_; }
    const Toolset& EditorContext::toolchain() const noexcept { return *toolset_; }
    asset::AssetVfsView EditorContext::vfs() const noexcept { return vfs_; }
    process::asset_loading::AssetReadPort EditorContext::assetRead() const noexcept { return asset_read_; }
    process::ExecutionRuntime& EditorContext::execution() noexcept { return *execution_; }
    const process::ExecutionRuntime& EditorContext::execution() const noexcept { return *execution_; }
    process::TaskScope& EditorContext::tasks() noexcept { return *tasks_; }
    const process::TaskScope& EditorContext::tasks() const noexcept { return *tasks_; }
    EditorSelection& EditorContext::selection() noexcept { return *selection_; }
    const EditorSelection& EditorContext::selection() const noexcept { return *selection_; }
    ui::UISession& EditorContext::ui() noexcept { return *ui_; }
    const ui::UISession& EditorContext::ui() const noexcept { return *ui_; }
    const scene::SceneMetaManager& EditorContext::sceneMeta() const noexcept { return *scene_meta_; }
} // namespace lux::editor
