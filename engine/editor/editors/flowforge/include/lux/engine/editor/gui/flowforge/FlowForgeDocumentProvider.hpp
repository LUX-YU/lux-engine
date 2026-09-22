#pragma once

#include <lux/engine/editor/flowforge/ui/visibility.h>
#include <lux/engine/editor/gui/GuiDocumentProvider.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>

namespace lux::editor::gui
{
[[nodiscard]] LUX_EDITOR_FLOWFORGE_UI_PUBLIC GuiDocumentProvider
    flowForgeDocumentProvider(lux::flowforge::FlowSourceEnvironment = {});
}
