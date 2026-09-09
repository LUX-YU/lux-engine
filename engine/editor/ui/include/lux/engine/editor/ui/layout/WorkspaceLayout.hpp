#pragma once
#include <lux/engine/editor/ui/shell/WindowSpec.hpp>
#include <lux/engine/ui/Layout.hpp>

namespace lux::editor::ui
{
    // The current generic SplitLayout already expresses the ER-1 recipe.
    struct WorkspaceLayout final
    {
        WorkspaceId workspace;
        std::uint32_t version{1};
        lux::ui::SplitLayout spec;
    };
} // namespace lux::editor::ui
