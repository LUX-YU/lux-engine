#pragma once

#include <lux/engine/editor/EditorSelection.hpp>
#include <lux/engine/editor/Toolset.hpp>
#include <lux/engine/editor/context/visibility.h>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>
#include <lux/engine/ui/UISession.hpp>

namespace lux::editor
{
    struct EditorContextCreateInfo final
    {
        Toolset& toolset;
        asset::AssetVfsView vfs;
        process::asset_loading::AssetReadPort asset_read;
        process::ExecutionRuntime& execution;
        process::TaskScope& tasks;
        EditorSelection& selection;
        ui::UISession& ui;
        const scene::SceneMetaManager& scene_meta;
    };

    /** Explicit, non-owning L5 capability aggregate for Editor windows. */
    class LUX_EDITOR_CONTEXT_PUBLIC EditorContext final
    {
    public:
        explicit EditorContext(EditorContextCreateInfo info) noexcept;

        EditorContext(const EditorContext&) = delete;
        EditorContext& operator=(const EditorContext&) = delete;
        EditorContext(EditorContext&&) = delete;
        EditorContext& operator=(EditorContext&&) = delete;

        [[nodiscard]] Toolset& toolchain() noexcept;
        [[nodiscard]] const Toolset& toolchain() const noexcept;
        [[nodiscard]] asset::AssetVfsView vfs() const noexcept;
        [[nodiscard]] process::asset_loading::AssetReadPort assetRead() const noexcept;
        [[nodiscard]] process::ExecutionRuntime& execution() noexcept;
        [[nodiscard]] const process::ExecutionRuntime& execution() const noexcept;
        [[nodiscard]] process::TaskScope& tasks() noexcept;
        [[nodiscard]] const process::TaskScope& tasks() const noexcept;
        [[nodiscard]] EditorSelection& selection() noexcept;
        [[nodiscard]] const EditorSelection& selection() const noexcept;
        [[nodiscard]] ui::UISession& ui() noexcept;
        [[nodiscard]] const ui::UISession& ui() const noexcept;
        [[nodiscard]] const scene::SceneMetaManager& sceneMeta() const noexcept;

    private:
        Toolset* toolset_{};
        asset::AssetVfsView vfs_;
        process::asset_loading::AssetReadPort asset_read_;
        process::ExecutionRuntime* execution_{};
        process::TaskScope* tasks_{};
        EditorSelection* selection_{};
        ui::UISession* ui_{};
        const scene::SceneMetaManager* scene_meta_{};
    };
} // namespace lux::editor
