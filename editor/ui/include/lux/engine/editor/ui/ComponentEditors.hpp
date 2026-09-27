#pragma once

#include <lux/engine/editor/metadata/ComponentEditorRegistry.hpp>
#include <lux/engine/editor/ui/visibility.h>

namespace lux::editor::ui
{
    // Product composition input, merged with plugin extensions once at startup.
    [[nodiscard]] LUX_EDITOR_UI_PUBLIC std::vector<ComponentEditorRegistration> componentEditors();
}
