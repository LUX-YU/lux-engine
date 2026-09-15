#pragma once

#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/editor/gui/visibility.h>
#include <lux/engine/editor/gui/scene/ComponentBinding.hpp>

namespace lux::editor::gui
{
    [[nodiscard]] LUX_EDITOR_GUI_PUBLIC GuiDocumentProvider sceneDocumentProvider(
    std::span<const lux::simulation::ecs::ComponentSchema> components = {},
    std::span<const ComponentBinding> bindings = {});
}
