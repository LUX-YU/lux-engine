#pragma once

#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/editor/scene/ui/visibility.h>
#include <lux/engine/editor/gui/scene/ComponentBinding.hpp>

namespace lux::editor::gui
{
    [[nodiscard]] LUX_EDITOR_SCENE_UI_PUBLIC GuiDocumentProvider sceneDocumentProvider(
    std::span<const lux::simulation::ecs::ComponentSchema> components = {},
    std::span<const ComponentBinding> bindings = {});
}
