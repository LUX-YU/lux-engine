#pragma once

#include <lux/engine/ui/Layout.hpp>
#include <string_view>

namespace lux::editor::ui
{
    // A visual recipe. It contains no document, resource or rendering owner.
    [[nodiscard]] lux::ui::SplitLayout sceneLayout(std::string_view pane_prefix);
} // namespace lux::editor::ui
