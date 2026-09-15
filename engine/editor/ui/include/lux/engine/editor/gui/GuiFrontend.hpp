#pragma once

#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/gui/shell/EditorWindow.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>

namespace lux::editor::gui
{
    struct GuiDocumentProvider final
    {
        std::string type;
        std::function<bool(const ProjectAssetEntry &)> accepts;
        std::function<EditorResult<void>(Editor &, process::ExecutionRuntime &, rendering::EditorRenderer &)>
            register_type;
        // Repeated opens retain existing panes and restore missing ones. A closing
        // instance must finish before its identity can be registered again.
        std::function<EditorResult<void>(DocumentEditor &, EditorWindow &, rendering::EditorRenderer &,
                                         process::ExecutionRuntime &)>
            attach;
    };

    struct GuiConfig final
    {
        WindowSpec window;
        rendering::RendererConfig renderer;
        std::vector<GuiDocumentProvider> providers;
    };

    [[nodiscard]] LUX_EDITOR_UI_PUBLIC std::unique_ptr<EditorFrontend> makeGuiFrontend(GuiConfig);
} // namespace lux::editor::gui
