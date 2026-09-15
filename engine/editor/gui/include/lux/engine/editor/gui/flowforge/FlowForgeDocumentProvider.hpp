#pragma once

#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/editor/gui/visibility.h>
#include <lux/engine/flowforge/graph/FlowSource.hpp>

namespace lux::editor::gui
{
    [[nodiscard]] LUX_EDITOR_GUI_PUBLIC GuiDocumentProvider flowForgeDocumentProvider(lux::flowforge::FlowSourceEnvironment = {});
}
