#pragma once

#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/ui/Layout.hpp>
#include <string_view>

namespace lux::editor::ui
{
    // A visual recipe. It contains no document, resource or rendering owner.
    [[nodiscard]] LUX_EDITOR_UI_PUBLIC lux::ui::SplitLayout sceneLayout(std::string_view pane_prefix);
} // namespace lux::editor::ui
