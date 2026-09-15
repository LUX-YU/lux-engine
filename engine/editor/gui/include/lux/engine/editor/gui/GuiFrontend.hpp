#pragma once

#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/gui/scene/ComponentBinding.hpp>
#include <lux/engine/editor/gui/shell/EditorWindow.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>

namespace lux::editor::gui
{
struct GuiDocumentProvider final
{
    std::string type;
    std::function<bool(const ProjectAssetEntry &)> accepts;
    std::function<EditorResult<void>(Editor &, process::ExecutionRuntime &, rendering::EditorRenderer &)> register_type;
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

[[nodiscard]] LUX_EDITOR_GUI_PUBLIC std::unique_ptr<EditorFrontend> makeGuiFrontend(GuiConfig);
[[nodiscard]] LUX_EDITOR_GUI_PUBLIC GuiDocumentProvider
sceneDocumentProvider(std::span<const lux::simulation::ecs::ComponentSchema> components = {},
                      std::span<const ComponentBinding> bindings = {});
[[nodiscard]] LUX_EDITOR_GUI_PUBLIC GuiDocumentProvider materialDocumentProvider();
[[nodiscard]] LUX_EDITOR_GUI_PUBLIC GuiDocumentProvider
    flowForgeDocumentProvider(lux::flowforge::FlowSourceEnvironment = {});
} // namespace lux::editor::gui
